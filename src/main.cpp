#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "mqtt/mqtt_client.h"
#include "provisioning/device_config.h"
#include "provisioning/register.h"
#include "ota/ota_updater.h"
#include "config/credential_rotation.h"
#include "wifi_setup/captive.h"

#ifdef FORGEKEY_TEMPERATURE_SENSOR
#include "temperature/temperature_sensor.h"
#else
#include "camera/camera_manager.h"
#include "detection/person_detector.h"
#include "counting/occupancy_counter.h"
#include "photo_upload/uploader.h"
#endif

// ============= CONFIGURATION =============
// WiFi credentials are no longer baked in: a first-boot captive portal
// (see src/wifi_setup/captive.cpp) collects them and persists them to NVS.
const char* MQTT_BROKER = "dms.oms-iot.com";
const int   MQTT_PORT = 1883;

const int DETECTION_INTERVAL = 2000;        // ms between detections
const int MQTT_PUBLISH_INTERVAL = 10000;    // ms between MQTT updates
// =========================================

// ============= STATUS LED =============
// Onboard LED on XIAO ESP32-S3 is GPIO 21 (orange LED)
const int STATUS_LED_PIN = 21;
const int LED_ON = LOW;   // LED is active LOW on XIAO ESP32-S3
const int LED_OFF = HIGH;

// LED blink patterns (on_ms, off_ms, repeat_count; 0 = infinite)
struct BlinkPattern {
    int onMs;
    int offMs;
    int count;
};

// Predefined patterns for different states
const BlinkPattern BOOT_PATTERN      = {100, 100, 5};   // Fast blink 5x at boot
const BlinkPattern WIFI_CONNECTING   = {500, 500, 0};   // Slow blink while connecting
const BlinkPattern NORMAL_PATTERN    = {50, 2950, 0};   // Short blink every 3s
const BlinkPattern ERROR_PATTERN     = {200, 200, 0};   // Fast blink (error)
const BlinkPattern MQTT_CONNECTED_PATTERN = {50, 50, 3}; // 3 quick blinks on MQTT connect

BlinkPattern currentPattern = BOOT_PATTERN;
unsigned long lastLedToggle = 0;
bool ledState = false;
int blinkCount = 0;
bool patternComplete = false;

void setLedPattern(const BlinkPattern& pattern) {
    currentPattern = pattern;
    blinkCount = 0;
    patternComplete = false;
    ledState = (pattern.onMs > 0) ? LED_ON : LED_OFF;
    digitalWrite(STATUS_LED_PIN, ledState);
    lastLedToggle = millis();
}

void updateLed() {
    if (patternComplete) return;

    unsigned long now = millis();
    unsigned long interval = ledState == LED_ON ? currentPattern.onMs : currentPattern.offMs;

    if (now - lastLedToggle >= interval) {
        ledState = !ledState;
        digitalWrite(STATUS_LED_PIN, ledState ? LED_ON : LED_OFF);
        lastLedToggle = now;

        if (ledState == LED_OFF) {
            blinkCount++;
            if (currentPattern.count > 0 && blinkCount >= currentPattern.count) {
                patternComplete = true;
                digitalWrite(STATUS_LED_PIN, LED_OFF);
            }
        }
    }
}

// ============= DEBUG HELPERS =============
void debugPrint(const char* level, const char* tag, const char* msg) {
    Serial.printf("[%6lu] [%s] %s: %s\n", millis(), level, tag, msg);
}

void debugPrintf(const char* level, const char* tag, const char* fmt, ...) {
    char buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Serial.printf("[%6lu] [%s] %s: %s\n", millis(), level, tag, buf);
}
// =========================================

unsigned long lastDetection = 0;
unsigned long lastMqttPublish = 0;
unsigned long lastTemperatureSample = 0;
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
            debugPrint("INFO", "CFG", "forget_wifi command received");
            WifiSetup::forgetAndRestart();  // does not return
            return;
        }
    }
    // Anything else (rotation payload, parse error, unknown cmd) falls through
    // to the credential-rotation handler, which logs and parses on its own.
    credential_rotation::onConfigMessage(topic, payload, length);
}

static void onFirmwareDispatch(const char* topic, const uint8_t* payload, unsigned int length) {
    debugPrintf("INFO", "OTA", "Dispatch on %s (%u bytes)", topic, length);
    OtaUpdater::Spec spec;
    if (!otaUpdater.parse(payload, length, spec)) {
        mqttClient.publishFirmwareStatus("failed", "", -1, "parse_error");
        return;
    }
    debugPrintf("INFO", "OTA", "Target version %s mandatory=%d",
                spec.version.c_str(), (int)spec.mandatory);
    mqttClient.publishFirmwareStatus("received", spec.version.c_str(), -1, nullptr);
#ifndef FORGEKEY_TEMPERATURE_SENSOR
    if (!spec.mandatory) {
        // Best-effort: defer if a photo upload was very recent.
        if (millis() - photoUploader.lastUploadMs() < 5000) {
            debugPrint("INFO", "OTA", "Deferring non-mandatory update — busy");
            mqttClient.publishFirmwareStatus("deferred", spec.version.c_str(), -1, "device_busy");
            return;
        }
    }
#endif
    otaUpdater.apply(spec);  // does not return on success
}

// First-boot: capture a photo (people-counter build) or send an empty body
// (temperature-sensor build) and POST to OMS, getting back our credentials.
// Returns true on success (credentials persisted).
static bool runProvisioning() {
#ifdef FORGEKEY_TEMPERATURE_SENSOR
    // Temperature sensors have no camera. The OMS register endpoint accepts
    // a zero-length photo part for non-imaging device kinds.
    debugPrint("INFO", "PROV", "Registering with OMS (no photo: temperature sensor)");
    return provisioning.registerDevice(OMS_HOST, OMS_PORT, macAddress,
                                       WiFi.localIP().toString(),
                                       nullptr, 0);
#else
    uint8_t* jpeg = nullptr;
    size_t jpegLen = 0;
    if (!cameraManager.captureJpeg(&jpeg, &jpegLen)) {
        debugPrint("ERROR", "PROV", "Photo capture failed");
        return false;
    }
    debugPrintf("INFO", "PROV", "Registering with OMS (photo: %u bytes)", jpegLen);
    bool ok = provisioning.registerDevice(OMS_HOST, OMS_PORT, macAddress,
                                          WiFi.localIP().toString(),
                                          jpeg, jpegLen);
    free(jpeg);
    return ok;
#endif
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    // Initialize status LED
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LED_OFF);
    setLedPattern(BOOT_PATTERN);

#ifdef FORGEKEY_TEMPERATURE_SENSOR
    debugPrint("INFO", "MAIN", "ForgeKey Temperature Sensor Starting...");
#else
    debugPrint("INFO", "MAIN", "ForgeKey People Counter Starting...");
#endif
    debugPrintf("INFO", "MAIN", "Firmware version: %s", FORGEKEY_FIRMWARE_VERSION);
    debugPrintf("INFO", "MAIN", "ESP32 SDK: %s", esp_get_idf_version());
    debugPrintf("INFO", "MAIN", "Free heap: %lu bytes", ESP.getFreeHeap());

    // Captive portal: blocks until the device is on WiFi, either via
    // previously-saved creds in NVS or via the AP+portal form.
    setLedPattern(WIFI_CONNECTING);
    debugPrint("INFO", "WIFI", "Connecting to WiFi...");
    if (!WifiSetup::connectOrPortal()) {
        debugPrint("ERROR", "WIFI", "Portal failed, restarting");
        setLedPattern(ERROR_PATTERN);
        delay(2000);
        ESP.restart();
    }
    debugPrint("INFO", "WIFI", "WiFi connected");
    debugPrintf("INFO", "WIFI", "IP: %s", WiFi.localIP().toString().c_str());
    debugPrintf("INFO", "WIFI", "RSSI: %d dBm", WiFi.RSSI());

    macAddress = WiFi.macAddress();
    macAddress.replace(":", "");
    macAddress.toLowerCase();
    debugPrintf("INFO", "MAIN", "MAC Address: %s", macAddress.c_str());

#ifdef FORGEKEY_TEMPERATURE_SENSOR
    debugPrintf("INFO", "TEMP", "Initializing DHT21 on GPIO %d...", FORGEKEY_DHT_PIN);
    if (!temperatureSensor.begin(FORGEKEY_DHT_PIN)) {
        debugPrint("ERROR", "TEMP", "DHT21 init failed!");
        setLedPattern(ERROR_PATTERN);
        return;
    }
    debugPrint("INFO", "TEMP", "DHT21 initialized");
#else
    debugPrint("INFO", "CAM", "Initializing camera...");
    if (!cameraManager.begin()) {
        debugPrint("ERROR", "CAM", "Camera init failed!");
        setLedPattern(ERROR_PATTERN);
        return;
    }
    debugPrint("INFO", "CAM", "Camera initialized");

    debugPrint("INFO", "DET", "Initializing person detector...");
    if (!personDetector.begin()) {
        debugPrint("ERROR", "DET", "Person detector init failed!");
        setLedPattern(ERROR_PATTERN);
        return;
    }
    debugPrint("INFO", "DET", "Person detector initialized");

    occupancyCounter.begin(0);
#endif

    provisioning.begin();
    otaUpdater.begin();
    // OTA layer is MQTT-agnostic; bridge its lifecycle events to the firmware
    // status topic so OMS can render "Downloading 60%", "Verifying", "Applied".
    otaUpdater.setStatusCallback([](const char* state,
                                    const char* version,
                                    int progress,
                                    const char* error) {
        mqttClient.publishFirmwareStatus(state, version, progress, error);
    });

    if (!provisioning.isProvisioned()) {
        debugPrint("INFO", "PROV", "First boot — registering with OMS");
        // Best-effort. If it fails we keep retrying on each boot; the
        // device still runs detection locally so on-prem operators see
        // something on the serial console.
        if (!runProvisioning()) {
            debugPrint("WARN", "PROV", "Registration failed; will retry on next boot");
        } else {
            debugPrint("INFO", "PROV", "Registration successful");
        }
    } else {
        debugPrint("INFO", "PROV", "Already provisioned");
    }

    DeviceCredentials creds = provisioning.credentials();
    const char* mqttJwt = creds.jwtToken.length() ? creds.jwtToken.c_str()
                                                   : "";
    mqttClient.begin(MQTT_BROKER, MQTT_PORT, mqttJwt);
    mqttClient.setTopicPrefix(macAddress.c_str());

    // Validate the stored pings topic against the OMS contract before
    // applying it. A topic from an older firmware (leading '/') or from a
    // bad registration response would otherwise silently override the
    // correct default. Required shape:
    //   forgekey/<bare-mac-12-hex>/(people_counter|door_counter)/occupancy
    //   forgekey/<bare-mac-12-hex>/temperature_sensor/reading
    auto isValidPingsTopic = [](const String& t) -> bool {
        if (!t.startsWith("forgekey/")) return false;
        // Expect exactly 4 segments after split: forgekey / <mac> / <kind> / <leaf>
        int s1 = t.indexOf('/');                    // after "forgekey"
        int s2 = (s1 >= 0) ? t.indexOf('/', s1+1) : -1;  // after <mac>
        int s3 = (s2 >= 0) ? t.indexOf('/', s2+1) : -1;  // after <kind>
        if (s1 < 0 || s2 < 0 || s3 < 0) return false;
        String mac = t.substring(s1+1, s2);
        String kind = t.substring(s2+1, s3);
        String leaf = t.substring(s3+1);
        if (mac.length() != 12) return false;
        for (size_t i = 0; i < mac.length(); ++i) {
            char ch = mac[i];
            bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
            if (!hex) return false;
        }
        if (kind == "people_counter" || kind == "door_counter") {
            return leaf == "occupancy";
        }
        if (kind == "temperature_sensor") {
            return leaf == "reading";
        }
        return false;
    };

    if (creds.mqttPingsTopic.length()) {
        if (isValidPingsTopic(creds.mqttPingsTopic)) {
#ifdef FORGEKEY_TEMPERATURE_SENSOR
            mqttClient.setReadingTopic(creds.mqttPingsTopic.c_str());
#else
            mqttClient.setOccupancyTopic(creds.mqttPingsTopic.c_str());
#endif
        } else {
            // Stale or malformed override (e.g. from pre-fix firmware). Don't
            // apply it; clear creds so the next boot re-registers and picks
            // up a clean topic from the server.
            debugPrintf("WARN", "MAIN",
                        "Stored mqtt_topic_for_pings='%s' does not match OMS contract; "
                        "ignoring and clearing creds to force re-register on next boot",
                        creds.mqttPingsTopic.c_str());
            provisioning.clear();
        }
    } else {
        debugPrint("INFO", "MAIN",
                   "No NVS mqtt_topic_for_pings; using firmware default forgekey/<mac>/<kind>/<leaf>");
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

#ifndef FORGEKEY_TEMPERATURE_SENSOR
    photoUploader.begin(OMS_HOST, OMS_PORT, macAddress);
    photoUploader.setJwt(creds.jwtToken);
#endif

    debugPrint("INFO", "MAIN", "Setup complete. Starting main loop...");
    setLedPattern(NORMAL_PATTERN);
}

void loop() {
    unsigned long now = millis();
    static bool mqttWasConnected = false;

    // Update status LED
    updateLed();

#ifndef FORGEKEY_TEMPERATURE_SENSOR
    // Run detection at specified interval
    if (now - lastDetection >= DETECTION_INTERVAL) {
        lastDetection = now;

        camera_fb_t* fb = cameraManager.capture();
        if (!fb) {
            debugPrint("ERROR", "CAM", "Camera capture failed");
            return;
        }

        DetectionResult result = personDetector.detect(fb->buf, fb->width, fb->height);
        occupancyCounter.updateCount(result.count);

        debugPrintf("INFO", "DET", "Detected: %d person(s), Confidence: %.2f, Motion: %s",
                    result.count, result.confidence, result.motionDetected ? "yes" : "no");

        if (result.motionDetected || result.count > 0) {
            photoUploader.markMotion(now);
        }

        cameraManager.returnFrame(fb);

        OccupancyData data = occupancyCounter.getData();
        if (data.changed) {
            occupancyChanged = true;
            debugPrintf("INFO", "CNT", "Occupancy changed to: %d", data.currentCount);
        }
    }
#endif

    // Check MQTT connection status and blink on (re)connect
    bool mqttConnected = mqttClient.isConnected();
    if (mqttConnected && !mqttWasConnected) {
        debugPrint("INFO", "MQTT", "MQTT connected");
        setLedPattern(MQTT_CONNECTED_PATTERN);
        // Resume normal pattern after the 3 blinks complete
        currentPattern = NORMAL_PATTERN;
        patternComplete = false;
        ledState = LED_OFF;
        digitalWrite(STATUS_LED_PIN, LED_OFF);
    } else if (!mqttConnected && mqttWasConnected) {
        debugPrint("WARN", "MQTT", "MQTT disconnected");
    }
    mqttWasConnected = mqttConnected;

#ifdef FORGEKEY_TEMPERATURE_SENSOR
    // Sample DHT21 + publish on the configured cadence.
    if (now - lastTemperatureSample >= TEMPERATURE_SAMPLE_INTERVAL_MS) {
        lastTemperatureSample = now;
        TemperatureReading r = temperatureSensor.read();
        if (r.ok) {
            debugPrintf("INFO", "TEMP", "Reading: %.2f C, %.2f%% RH", r.tempC, r.humidity);
            if (mqttConnected) {
                if (mqttClient.publishTemperature(r.tempC, r.humidity)) {
                    lastMqttPublish = now;
                    debugPrint("INFO", "MQTT", "Published temperature reading");
                    // First successful publish post-OTA = bless the partition.
                    otaUpdater.markStableIfPending();
                } else {
                    debugPrint("ERROR", "MQTT", "Failed to publish temperature reading");
                }
            } else {
                debugPrint("WARN", "MQTT", "Skipping publish — MQTT not connected");
            }
        }
    }
#else
    // Publish to MQTT (on change or at interval)
    if (mqttConnected &&
        (occupancyChanged || now - lastMqttPublish >= MQTT_PUBLISH_INTERVAL)) {
        int count = occupancyCounter.getCurrentCount();
        if (mqttClient.publishOccupancy(count)) {
            lastMqttPublish = now;
            occupancyChanged = false;
            debugPrintf("INFO", "MQTT", "Published occupancy: %d", count);
            // First successful publish post-OTA = green light to bless the partition.
            otaUpdater.markStableIfPending();
        } else {
            debugPrint("ERROR", "MQTT", "Failed to publish occupancy");
        }
    }

    // Periodic photo upload (bandwidth-gated by motion).
    if (provisioning.isProvisioned() && !otaUpdater.inProgress() &&
        photoUploader.shouldUpload(now, PHOTO_UPLOAD_INTERVAL_MS,
                                   PHOTO_UPLOAD_MOTION_WINDOW_MS)) {
        uint8_t* jpeg = nullptr;
        size_t jpegLen = 0;
        if (cameraManager.captureJpeg(&jpeg, &jpegLen)) {
            debugPrintf("INFO", "UPLOAD", "Uploading photo (%u bytes)", jpegLen);
            PhotoUploader::Result r = photoUploader.uploadPhoto(jpeg, jpegLen, now);
            free(jpeg);
            if (r == PhotoUploader::Result::AuthExpired) {
                debugPrint("WARN", "UPLOAD", "Auth expired, will re-register on next boot");
                // Force a re-register on the next boot rather than blocking
                // the loop here. The next photo cycle will pick up new creds.
                provisioning.clear();
            } else if (r == PhotoUploader::Result::Ok) {
                debugPrint("INFO", "UPLOAD", "Photo upload successful");
            } else {
                debugPrint("ERROR", "UPLOAD", "Photo upload failed");
            }
        }
    }
#endif

    mqttClient.loop();
    delay(10);
}
