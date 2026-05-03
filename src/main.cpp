#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>

#include "mqtt/mqtt_client.h"
#include "provisioning/device_config.h"
#include "provisioning/register.h"
#include "ota/ota_updater.h"
#include "config/credential_rotation.h"
#include "wifi_setup/captive.h"

#include "capabilities/registry.h"
#include "capabilities/status_led/status_led.h"

#ifndef FORGEKEY_TEMPERATURE_SENSOR
// People-counter build pulls in the camera-based occupancy capability and
// uses a per-device photo for first-boot OMS registration.
#include "capabilities/people_counter/people_counter.h"
#include "photo_upload/uploader.h"
#endif

// ============= CONFIGURATION =============
// WiFi credentials are no longer baked in: a first-boot captive portal
// (see src/wifi_setup/captive.cpp) collects them and persists them to NVS.
const char* MQTT_BROKER = "dms.oms-iot.com";
const int   MQTT_PORT = 1883;
// =========================================

String macAddress;

// ============= DEBUG HELPERS =============
void debugPrint(const char* level, const char* tag, const char* msg) {
    Serial.printf("[%6lu] [%s] %s: %s\n", millis(), level, tag, msg);
}

void debugPrintf(const char* level, const char* tag, const char* fmt, ...) {
    char buf[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Serial.printf("[%6lu] [%s] %s: %s\n", millis(), level, tag, buf);
}
// =========================================

// Operator-triggered "identify me" blink. Routes through the status_led
// capability's override channel; status echoes are mirrored to the OMS
// device-control panel via the MQTT status topic.
static void setBlinkActive(bool on) {
    if (!StatusLed::setBlinkOverride(on)) return;  // no-op: already in that state
    mqttClient.publishBlinkStatus(on);
    debugPrintf("INFO", "CMD", "blink -> %s", on ? "on" : "off");
}

static void onCommandMessage(const char* topic, const uint8_t* payload, unsigned int length) {
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
        debugPrintf("WARN", "CMD", "parse error: %s", err.c_str());
        return;
    }
    const char* cmd = doc["cmd"] | "";
    if (strcmp(cmd, "blink") == 0) {
        // Three accepted forms:
        //   {"cmd":"blink"}                     -> toggle (back-compat)
        //   {"cmd":"blink","action":"start|stop|toggle"}
        //   {"cmd":"blink","on":true|false}
        if (doc.containsKey("on")) {
            setBlinkActive(doc["on"].as<bool>());
        } else if (doc.containsKey("action")) {
            const char* action = doc["action"] | "";
            if (strcmp(action, "start") == 0) {
                setBlinkActive(true);
            } else if (strcmp(action, "stop") == 0) {
                setBlinkActive(false);
            } else if (strcmp(action, "toggle") == 0) {
                setBlinkActive(!StatusLed::blinkOverrideActive());
            } else {
                debugPrintf("WARN", "CMD", "unknown blink action: %s", action);
            }
        } else {
            setBlinkActive(!StatusLed::blinkOverrideActive());
        }
        return;
    }
    debugPrintf("WARN", "CMD", "unknown cmd: '%s'", cmd);
}


// Single dispatcher for the shared forgekey/<mac>/config topic. The two
// payload shapes coexist:
//   {"cmd": "forget_wifi"}                     -> clear WiFi creds, reboot
//   {"provisioning_token": "...", "valid_after": "..."}  -> token rotation
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
    // Anything else (rotation payload, parse error, unknown cmd) falls
    // through to the credential-rotation handler.
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

// First-boot: capture an artifact (people-counter photo if camera detected,
// otherwise empty body) and POST to OMS, getting back our credentials.
// Returns true on success (credentials persisted).
static bool runProvisioning() {
    uint8_t* jpeg = nullptr;
    size_t jpegLen = 0;
    bool havePhoto = false;

#ifndef FORGEKEY_TEMPERATURE_SENSOR
    if (PeopleCounter::isActive() &&
        PeopleCounter::captureProvisioningPhoto(&jpeg, &jpegLen)) {
        havePhoto = true;
        debugPrintf("INFO", "PROV", "Registering with OMS (photo: %u bytes)",
                    (unsigned)jpegLen);
    } else {
        debugPrint("INFO", "PROV",
                   "Registering with OMS (no photo: people-counter capability not active)");
    }
#else
    // Temperature builds have no camera. The OMS register endpoint accepts a
    // zero-length photo part for non-imaging device kinds.
    debugPrint("INFO", "PROV", "Registering with OMS (no photo: temperature build)");
#endif

    bool ok = provisioning.registerDevice(OMS_HOST, OMS_PORT, macAddress,
                                          WiFi.localIP().toString(),
                                          havePhoto ? jpeg : nullptr,
                                          havePhoto ? jpegLen : 0);
    if (havePhoto) free(jpeg);
    return ok;
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    debugPrint("INFO", "MAIN", "ForgeKey Starting...");
    debugPrintf("INFO", "MAIN", "Firmware version: %s", FORGEKEY_FIRMWARE_VERSION);
#ifdef FIRMWARE_GIT_COMMIT
    debugPrintf("INFO", "MAIN", "Git commit: %s", FIRMWARE_GIT_COMMIT);
#endif
#ifdef FIRMWARE_BUILD_TIMESTAMP
    debugPrintf("INFO", "MAIN", "Build timestamp (epoch): %s", FIRMWARE_BUILD_TIMESTAMP);
#endif
    debugPrintf("INFO", "MAIN", "Compiled: %s %s", __DATE__, __TIME__);
    debugPrintf("INFO", "MAIN", "ESP32 SDK: %s", esp_get_idf_version());
    debugPrintf("INFO", "MAIN", "Free heap: %lu bytes", ESP.getFreeHeap());
    {
        // MAC: log the firmware-computed value early, before any provisioning
        // step can produce its own. This is the source-of-truth byte sequence
        // used in topic derivation.
        String earlyMac = WiFi.macAddress();
        debugPrintf("INFO", "MAIN", "WiFi.macAddress() (pre-connect): %s", earlyMac.c_str());
    }

    // Capability detection runs before WiFi: pure hardware probes. Any
    // capability whose detect() returns true gets setup() called immediately
    // afterwards. From this point on the LED state machine is live.
    debugPrint("INFO", "MAIN", "Detecting capabilities...");
    CapabilityRegistry::detectAll();
    CapabilityRegistry::setupAll();
    debugPrintf("INFO", "MAIN", "Capabilities: %d/%d active",
                CapabilityRegistry::activeCount(), CapabilityRegistry::count());

    StatusLed::requestState(StatusLed::State::WifiConnecting);
    debugPrint("INFO", "WIFI", "Connecting to WiFi...");
    if (!WifiSetup::connectOrPortal()) {
        debugPrint("ERROR", "WIFI", "Portal failed, restarting");
        StatusLed::requestState(StatusLed::State::Error);
        delay(2000);
        ESP.restart();
    }
    debugPrint("INFO", "WIFI", "WiFi connected");
    debugPrintf("INFO", "WIFI", "SSID: %s", WiFi.SSID().c_str());
    debugPrintf("INFO", "WIFI", "BSSID: %s channel=%d",
                WiFi.BSSIDstr().c_str(), WiFi.channel());
    debugPrintf("INFO", "WIFI", "IP: %s", WiFi.localIP().toString().c_str());
    debugPrintf("INFO", "WIFI", "Subnet: %s", WiFi.subnetMask().toString().c_str());
    debugPrintf("INFO", "WIFI", "Gateway: %s", WiFi.gatewayIP().toString().c_str());
    debugPrintf("INFO", "WIFI", "DNS: %s / %s",
                WiFi.dnsIP(0).toString().c_str(),
                WiFi.dnsIP(1).toString().c_str());
    debugPrintf("INFO", "WIFI", "RSSI: %d dBm", WiFi.RSSI());

    macAddress = WiFi.macAddress();
    macAddress.replace(":", "");
    macAddress.toLowerCase();
    debugPrintf("INFO", "MAIN", "MAC Address: %s", macAddress.c_str());

    provisioning.begin();
    otaUpdater.begin();
    otaUpdater.setStatusCallback([](const char* state,
                                    const char* version,
                                    int progress,
                                    const char* error) {
        mqttClient.publishFirmwareStatus(state, version, progress, error);
    });

    if (!provisioning.isProvisioned()) {
        debugPrint("INFO", "PROV", "First boot — registering with OMS");
        if (!runProvisioning()) {
            debugPrint("WARN", "PROV", "Registration failed; will retry on next boot");
        } else {
            debugPrint("INFO", "PROV", "Registration successful");
        }
    } else {
        debugPrint("INFO", "PROV", "Already provisioned");
    }

    DeviceCredentials creds = provisioning.credentials();
    const char* mqttJwt = creds.jwtToken.length() ? creds.jwtToken.c_str() : "";
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
        int s1 = t.indexOf('/');
        int s2 = (s1 >= 0) ? t.indexOf('/', s1+1) : -1;
        int s3 = (s2 >= 0) ? t.indexOf('/', s2+1) : -1;
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

    String configTopic = credential_rotation::topicFor(macAddress);
    mqttClient.setConfigTopic(configTopic.c_str());
    mqttClient.subscribeConfig(onConfigMessage);

    // Per-device control plane (operator commands + state echo). MAC-derived
    // for the same reason as configTopic — OMS can address a device before it
    // has an id assigned.
    String commandTopic = String("forgekey/") + macAddress + "/command";
    String statusTopic  = String("forgekey/") + macAddress + "/status";
    mqttClient.setCommandTopic(commandTopic.c_str());
    mqttClient.setStatusTopic(statusTopic.c_str());
    mqttClient.subscribeCommand(onCommandMessage);

#ifndef FORGEKEY_TEMPERATURE_SENSOR
    photoUploader.begin(OMS_HOST, OMS_PORT, macAddress);
    photoUploader.setJwt(creds.jwtToken);
#endif

    StatusLed::requestState(StatusLed::State::Normal);
    debugPrint("INFO", "MAIN", "Setup complete. Starting main loop...");
}

void loop() {
    static bool mqttWasConnected = false;
    static bool capabilitiesAnnounced = false;

    bool mqttConnected = mqttClient.isConnected();
    if (mqttConnected && !mqttWasConnected) {
        debugPrint("INFO", "MQTT", "MQTT connected");
        // status_led's tick() preserves the operator blink override across
        // this transition (a reconnect during identify shouldn't silently stop
        // the blink), so we can request the burst unconditionally.
        StatusLed::requestState(StatusLed::State::MqttConnected);
    } else if (!mqttConnected && mqttWasConnected) {
        // Log the underlying state so we can distinguish broker-initiated vs
        // network-initiated drops. Time since last successful publish helps
        // tell "broker dropped us idle" from "we just lost the socket".
        unsigned long now = millis();
        unsigned long lastPub = mqttClient.lastSuccessfulPublishMs();
        unsigned long sinceMs = (lastPub == 0) ? 0 : (now - lastPub);
        debugPrintf("WARN", "MQTT",
                    "MQTT disconnected: last_state=%d since_last_publish_ms=%s",
                    mqttClient.lastConnectRc(),
                    (lastPub == 0) ? "never" : String(sinceMs).c_str());
    }
    mqttWasConnected = mqttConnected;

    // One-time capability announcement after first MQTT connect. Retained=true
    // means the broker delivers the latest payload to OMS on subscribe, so a
    // reconnect doesn't need to re-publish unless the active set changes
    // (out of scope for V1 — capabilities are detected once at boot).
    if (mqttConnected && !capabilitiesAnnounced) {
        String json = CapabilityRegistry::announcementJson(FORGEKEY_FIRMWARE_VERSION);
        debugPrintf("INFO", "CAP", "Announcing capabilities: %s", json.c_str());
        if (mqttClient.publishCapabilities(json.c_str())) {
            capabilitiesAnnounced = true;
        }
    }

    CapabilityRegistry::tickAll();

    mqttClient.loop();
    delay(10);
}
