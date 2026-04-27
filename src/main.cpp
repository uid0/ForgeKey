#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "camera/camera_manager.h"
#include "detection/person_detector.h"
#include "counting/occupancy_counter.h"
#include "mqtt/mqtt_client.h"
#include "provisioning/device_config.h"
#include "provisioning/register.h"
#include "photo_upload/uploader.h"
#include "ota/ota_updater.h"
#include "config/credential_rotation.h"
#include "wifi_setup/captive.h"

// ============= CONFIGURATION =============
// WiFi credentials are no longer baked in: a first-boot captive portal
// (see src/wifi_setup/captive.cpp) collects them and persists them to NVS.
const char* MQTT_BROKER = "mqtt.yourdomain.com";
const int   MQTT_PORT = 1883;

const int DETECTION_INTERVAL = 2000;        // ms between detections
const int MQTT_PUBLISH_INTERVAL = 10000;    // ms between MQTT updates
// =========================================

unsigned long lastDetection = 0;
unsigned long lastMqttPublish = 0;
bool occupancyChanged = false;
String macAddress;

// Single dispatcher for the shared forgekey/<mac>/config topic. The two
// payload shapes coexist:
//   {"cmd": "forget_wifi"}                     → clear WiFi creds, reboot
//   {"provisioning_token": "...", "valid_after": "..."}  → token rotation
static void onConfigMessage(const char* topic, const uint8_t* payload, unsigned int length) {
    StaticJsonDocument<384> doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (!err) {
        const char* cmd = doc["cmd"] | "";
        if (strcmp(cmd, "forget_wifi") == 0) {
            Serial.println("config: forget_wifi command received");
            WifiSetup::forgetAndRestart();  // does not return
            return;
        }
    }
    // Anything else (rotation payload, parse error, unknown cmd) falls through
    // to the credential-rotation handler, which logs and parses on its own.
    credential_rotation::onConfigMessage(topic, payload, length);
}

static void onFirmwareDispatch(const char* topic, const uint8_t* payload, unsigned int length) {
    Serial.printf("ota: dispatch on %s (%u bytes)\n", topic, length);
    OtaUpdater::Spec spec;
    if (!otaUpdater.parse(payload, length, spec)) return;
    Serial.printf("ota: target version %s mandatory=%d\n",
                  spec.version.c_str(), (int)spec.mandatory);
    if (!spec.mandatory) {
        // Best-effort: defer if a photo upload was very recent.
        if (millis() - photoUploader.lastUploadMs() < 5000) {
            Serial.println("ota: deferring non-mandatory update — busy");
            return;
        }
    }
    otaUpdater.apply(spec);  // does not return on success
}

// First-boot: capture a photo and send it to OMS, getting back our credentials.
// Returns true on success (credentials persisted).
static bool runProvisioning() {
    uint8_t* jpeg = nullptr;
    size_t jpegLen = 0;
    if (!cameraManager.captureJpeg(&jpeg, &jpegLen)) {
        Serial.println("provision: photo capture failed");
        return false;
    }
    bool ok = provisioning.registerDevice(OMS_HOST, OMS_PORT, macAddress,
                                          WiFi.localIP().toString(),
                                          jpeg, jpegLen);
    free(jpeg);
    return ok;
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\nForgeKey People Counter Starting...");
    Serial.printf("Firmware version: %s\n", FORGEKEY_FIRMWARE_VERSION);

    // Captive portal: blocks until the device is on WiFi, either via
    // previously-saved creds in NVS or via the AP+portal form.
    if (!WifiSetup::connectOrPortal()) {
        Serial.println("WiFi: portal failed, restarting");
        delay(2000);
        ESP.restart();
    }
    Serial.println("WiFi connected");
    Serial.println("IP: " + WiFi.localIP().toString());

    macAddress = WiFi.macAddress();
    macAddress.replace(":", "");
    macAddress.toLowerCase();

    if (!cameraManager.begin()) {
        Serial.println("Camera init failed!");
        return;
    }
    if (!personDetector.begin()) {
        Serial.println("Person detector init failed!");
        return;
    }
    occupancyCounter.begin(0);

    provisioning.begin();
    otaUpdater.begin();

    if (!provisioning.isProvisioned()) {
        Serial.println("provision: first boot — registering with OMS");
        // Best-effort. If it fails we keep retrying on each boot; the
        // device still runs detection locally so on-prem operators see
        // something on the serial console.
        if (!runProvisioning()) {
            Serial.println("provision: registration failed; will retry on next boot");
        }
    }

    DeviceCredentials creds = provisioning.credentials();
    const char* mqttJwt = creds.jwtToken.length() ? creds.jwtToken.c_str()
                                                   : "";
    mqttClient.begin(MQTT_BROKER, MQTT_PORT, mqttJwt);
    mqttClient.setTopicPrefix(macAddress.c_str());
    if (creds.mqttPingsTopic.length()) {
        mqttClient.setOccupancyTopic(creds.mqttPingsTopic.c_str());
    }
    if (creds.mqttFirmwareTopic.length()) {
        mqttClient.setFirmwareTopic(creds.mqttFirmwareTopic.c_str());
        mqttClient.subscribeFirmware(onFirmwareDispatch);
    }

    // Config / control topic. Topic is derived from the MAC rather than
    // handed back at registration so the back-end can fan out messages
    // (token rotation, forget_wifi) before knowing each device's OMS id.
    String configTopic = credential_rotation::topicFor(macAddress);
    mqttClient.setConfigTopic(configTopic.c_str());
    mqttClient.subscribeConfig(onConfigMessage);

    photoUploader.begin(OMS_HOST, OMS_PORT, macAddress);
    photoUploader.setJwt(creds.jwtToken);

    Serial.println("Setup complete. Starting detection loop...");
}

void loop() {
    unsigned long now = millis();

    // Run detection at specified interval
    if (now - lastDetection >= DETECTION_INTERVAL) {
        lastDetection = now;

        camera_fb_t* fb = cameraManager.capture();
        if (!fb) {
            Serial.println("Camera capture failed");
            return;
        }

        DetectionResult result = personDetector.detect(fb->buf, fb->width, fb->height);
        occupancyCounter.updateCount(result.count);

        Serial.printf("Detected: %d person(s), Confidence: %.2f\n",
                     result.count, result.confidence);

        if (result.motionDetected || result.count > 0) {
            photoUploader.markMotion(now);
        }

        cameraManager.returnFrame(fb);

        OccupancyData data = occupancyCounter.getData();
        if (data.changed) {
            occupancyChanged = true;
            Serial.printf("Occupancy changed to: %d\n", data.currentCount);
        }
    }

    // Publish to MQTT (on change or at interval)
    if (mqttClient.isConnected() &&
        (occupancyChanged || now - lastMqttPublish >= MQTT_PUBLISH_INTERVAL)) {
        int count = occupancyCounter.getCurrentCount();
        if (mqttClient.publishOccupancy(count)) {
            lastMqttPublish = now;
            occupancyChanged = false;
            // First successful publish post-OTA = green light to bless the partition.
            otaUpdater.markStableIfPending();
        }
    }

    // Periodic photo upload (bandwidth-gated by motion).
    if (provisioning.isProvisioned() && !otaUpdater.inProgress() &&
        photoUploader.shouldUpload(now, PHOTO_UPLOAD_INTERVAL_MS,
                                   PHOTO_UPLOAD_MOTION_WINDOW_MS)) {
        uint8_t* jpeg = nullptr;
        size_t jpegLen = 0;
        if (cameraManager.captureJpeg(&jpeg, &jpegLen)) {
            PhotoUploader::Result r = photoUploader.uploadPhoto(jpeg, jpegLen, now);
            free(jpeg);
            if (r == PhotoUploader::Result::AuthExpired) {
                // Force a re-register on the next boot rather than blocking
                // the loop here. The next photo cycle will pick up new creds.
                provisioning.clear();
            }
        }
    }

    mqttClient.loop();
    delay(10);
}
