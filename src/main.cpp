#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <nvs_flash.h>
#include <time.h>

#include "mqtt/mqtt_client.h"
#include "provisioning/device_config.h"
#include "provisioning/register.h"
#include "ota/ota_updater.h"
#include "config/credential_rotation.h"
#include "config/wifi_desired_state.h"
#include "config/ble_desired_state.h"
#include "security/command_validation.h"
#include "time/time_sync.h"
#include "wifi_setup/captive.h"

#include "capabilities/registry.h"
#include "capabilities/status_led/status_led.h"
#include "boards/board_manifest.h"

#ifndef FORGEKEY_DISABLE_BLE_SCANNER
#include "capabilities/ble_scanner/ble_scanner.h"
#endif
#ifndef FORGEKEY_DISABLE_BLE_BEACON
#include "capabilities/ble_beacon/ble_beacon.h"
#endif
#ifndef FORGEKEY_DISABLE_BLE_RELAY
#include "capabilities/ble_relay/ble_relay.h"
#endif
#ifndef FORGEKEY_DISABLE_BLE_EQUIPMENT
#include "capabilities/ble_equipment/ble_equipment.h"
#endif

#if !defined(FORGEKEY_TEMPERATURE_SENSOR) && !defined(FORGEKEY_EPAPER)
// People-counter build pulls in the camera-based occupancy capability and
// uses a per-device photo for first-boot OMS enrollment.
#include "capabilities/people_counter/people_counter.h"
#include "photo_upload/uploader.h"
#endif

#ifdef FORGEKEY_LOCK
// Cabinet-lock build: non-blocking state machine for lock/unlock operations
// with MQTT signaling, signed-command validation, and embedded status web page.
#include "lock/lock_capability.h"
#include "lock/embedded_web.h"
#endif

// ============= CONFIGURATION =============
// WiFi credentials are no longer baked in: a first-boot captive portal
// (see src/wifi_setup/captive.cpp) collects them and persists them to NVS.
// MQTT broker connection info is no longer baked in either: the OMS
// enrollment response supplies it (mqtt_broker_host / _port / _use_tls)
// and we cache it in NVS. Pre-enrollment boots use the compile-time
// MQTT_BROKER_FALLBACK_* values from provisioning/device_config.h.
// =========================================

String macAddress;

void debugPrint(const char* level, const char* tag, const char* msg);
void debugPrintf(const char* level, const char* tag, const char* fmt, ...);

// ============= DEBUG HELPERS =============
namespace {
constexpr size_t kDebugLevelMax = 8;
constexpr size_t kDebugTagMax = 16;
constexpr size_t kDebugMessageMax = 160;
constexpr size_t kBufferedDebugLogCount = 24;
constexpr unsigned long kOtaRapidCheckDurationMs = 15UL * 60UL * 1000UL;
constexpr unsigned long kOtaRapidCheckIntervalMs = 60UL * 1000UL;

unsigned long g_otaRapidCheckUntilMs = 0;
unsigned long g_nextOtaRapidCheckMs = 0;

struct BufferedDebugLog {
    unsigned long timestampMs;
    char level[kDebugLevelMax];
    char tag[kDebugTagMax];
    char message[kDebugMessageMax];
};

BufferedDebugLog g_bufferedDebugLogs[kBufferedDebugLogCount];
size_t g_bufferedDebugLogStart = 0;
size_t g_bufferedDebugLogCount = 0;

void copyDebugField(char* dest, size_t destSize, const char* src) {
    if (destSize == 0) return;
    snprintf(dest, destSize, "%s", src ? src : "");
}

void bufferDebugLog(unsigned long timestampMs,
                    const char* level,
                    const char* tag,
                    const char* message) {
    size_t index;
    if (g_bufferedDebugLogCount < kBufferedDebugLogCount) {
        index = (g_bufferedDebugLogStart + g_bufferedDebugLogCount) % kBufferedDebugLogCount;
        g_bufferedDebugLogCount++;
    } else {
        index = g_bufferedDebugLogStart;
        g_bufferedDebugLogStart = (g_bufferedDebugLogStart + 1) % kBufferedDebugLogCount;
    }

    g_bufferedDebugLogs[index].timestampMs = timestampMs;
    copyDebugField(g_bufferedDebugLogs[index].level,
                   sizeof(g_bufferedDebugLogs[index].level), level);
    copyDebugField(g_bufferedDebugLogs[index].tag,
                   sizeof(g_bufferedDebugLogs[index].tag), tag);
    copyDebugField(g_bufferedDebugLogs[index].message,
                   sizeof(g_bufferedDebugLogs[index].message), message);
}

void flushBufferedDebugLogs() {
    while (g_bufferedDebugLogCount > 0) {
        BufferedDebugLog& entry = g_bufferedDebugLogs[g_bufferedDebugLogStart];
        if (!mqttClient.publishLog(entry.timestampMs, entry.level, entry.tag, entry.message)) {
            return;
        }
        g_bufferedDebugLogStart = (g_bufferedDebugLogStart + 1) % kBufferedDebugLogCount;
        g_bufferedDebugLogCount--;
    }
}

void emitDebugLog(const char* level, const char* tag, const char* message) {
    unsigned long timestampMs = millis();
    Serial.printf("[%6lu] [%s] %s: %s\n", timestampMs, level, tag, message);
    if (!mqttClient.publishLog(timestampMs, level, tag, message)) {
        bufferDebugLog(timestampMs, level, tag, message);
    }
}

bool timeReached(unsigned long nowMs, unsigned long targetMs) {
    return (long)(nowMs - targetMs) >= 0;
}

void extendOtaRapidChecking(const char* reason) {
    unsigned long now = millis();
    unsigned long newDeadline = now + kOtaRapidCheckDurationMs;
    if (g_otaRapidCheckUntilMs == 0 || timeReached(now, g_otaRapidCheckUntilMs) ||
        timeReached(newDeadline, g_otaRapidCheckUntilMs)) {
        g_otaRapidCheckUntilMs = newDeadline;
    }
    g_nextOtaRapidCheckMs = now;
    Serial.printf("ota: rapid checking active for ~15 minutes (%s)\n",
                  reason ? reason : "scheduled");
}

void tickOtaRapidChecking(bool mqttConnected) {
    if (g_otaRapidCheckUntilMs == 0) return;

    unsigned long now = millis();
    if (timeReached(now, g_otaRapidCheckUntilMs)) {
        g_otaRapidCheckUntilMs = 0;
        Serial.println("ota: rapid checking window ended");
        return;
    }
    if (!mqttConnected || !timeReached(now, g_nextOtaRapidCheckMs)) return;

    if (mqttClient.refreshFirmwareSubscription()) {
        debugPrint("INFO", "OTA", "Rapid firmware check refreshed subscription");
    }
    g_nextOtaRapidCheckMs = now + kOtaRapidCheckIntervalMs;
}
}  // namespace

void debugPrint(const char* level, const char* tag, const char* msg) {
    emitDebugLog(level, tag, msg ? msg : "");
}

void debugPrintf(const char* level, const char* tag, const char* fmt, ...) {
    char buf[kDebugMessageMax];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    emitDebugLog(level, tag, buf);
}
// =========================================

// Operator-triggered "identify me" blink. Routes through the status_led
// capability's override channel; status echoes are mirrored to the OMS
// device-control panel via the MQTT status topic.
static void setBlinkActive(bool on) {
    if (!StatusLed::setBlinkOverride(on)) return;  // no-op: already in that state
    mqttClient.publishBlinkStatus(on);
    StatusLed::triggerMessageFlash();
    debugPrintf("INFO", "CMD", "blink -> %s", on ? "on" : "off");
}

// Default identify duration when {"cmd":"identify"} is sent without
// duration_s. Visible-but-not-annoying window for an operator to walk over
// to the device.
#ifndef IDENTIFY_DEFAULT_DURATION_S
#define IDENTIFY_DEFAULT_DURATION_S 30
#endif

static void publishStatusSnapshot(const char* requestedCmd, const char* commandId) {
    // Used for {"cmd":"status"} and {"cmd":"ping"} acks. We normalize
    // cmd_ack to "status" for both so OMS/UI callers can treat ping as a
    // status alias while still seeing which command triggered the snapshot.
    // Includes enough info for an operator to decide "is the device alive
    // and what does it think it can do" without needing it to actively
    // report.
    String capsJson = CapabilityRegistry::announcementJson(FORGEKEY_FIRMWARE_VERSION);
    // capsJson looks like: {"capabilities":[...],"firmware_version":"x"}
    // Trim the outer braces so we can splice the contents into our payload.
    String capsInner;
    if (capsJson.length() >= 2 && capsJson[0] == '{' &&
        capsJson[capsJson.length() - 1] == '}') {
        capsInner = capsJson.substring(1, capsJson.length() - 1);
    } else {
        capsInner = "";
    }
    String payload;
    payload.reserve(320);
    payload += "{\"cmd_ack\":\"status\"";
    payload += ",\"command_id\":\"";
    payload += (commandId ? commandId : "");
    payload += "\"";
    if (requestedCmd && *requestedCmd) {
        payload += ",\"requested_cmd\":\"";
        payload += requestedCmd;
        payload += "\"";
    }
    payload += ",";
    payload += capsInner;
    payload += ",\"free_heap\":";
    payload += String((unsigned long)ESP.getFreeHeap());
    payload += ",\"rssi\":";
    payload += String(WiFi.RSSI());
    WifiSetup::appendHealthJson(payload);
    BoardManifest::appendHealthJson(payload, CapabilityRegistry::head());
    payload += ",\"ble_config\":";
    ble_desired_state::appendConfigJson(payload);
    payload += ",\"capability_health\":{\"ble\":{";
    bool bleHealthAdded = false;
#ifndef FORGEKEY_DISABLE_BLE_SCANNER
    payload += "\"scanner\":";
    BleScanner::appendHealthJson(payload);
    bleHealthAdded = true;
#endif
#ifndef FORGEKEY_DISABLE_BLE_BEACON
    if (bleHealthAdded) payload += ",";
    payload += "\"beacon\":";
    BleBeacon::appendHealthJson(payload);
    bleHealthAdded = true;
#endif
#ifndef FORGEKEY_DISABLE_BLE_RELAY
    if (bleHealthAdded) payload += ",";
    payload += "\"relay\":";
    BleRelay::appendHealthJson(payload);
    bleHealthAdded = true;
#endif
#ifndef FORGEKEY_DISABLE_BLE_EQUIPMENT
    if (bleHealthAdded) payload += ",";
    payload += "\"equipment\":";
    BleEquipment::appendHealthJson(payload);
#endif
    payload += "}}";
    payload += ",\"uptime_ms\":";
    payload += String(millis());
    ForgeKeyTime::appendJson(payload);
    payload += ",\"mac\":\"";
    payload += macAddress;
    payload += "\"";
    OtaUpdater::appendHealthJson(payload);
    payload += "}";
    mqttClient.publishStatus(payload.c_str());
    StatusLed::triggerMessageFlash();
}

static void publishUnknownCommandAck(const char* cmd, const char* commandId) {
    String payload;
    payload.reserve(64 + strlen(cmd ? cmd : ""));
    payload += "{\"cmd_ack\":\"";
    payload += (cmd ? cmd : "");
    payload += "\",\"command_id\":\"";
    payload += (commandId ? commandId : "");
    payload += "\",\"error\":\"unknown_command\"}";
    mqttClient.publishStatus(payload.c_str());
    StatusLed::triggerMessageFlash();
}

static void publishCommandRejectAck(const char* cmd, const char* commandId,
                                    const char* error, const char* detail = nullptr) {
    StaticJsonDocument<256> ack;
    ack["cmd_ack"] = cmd ? cmd : "";
    ack["command_id"] = commandId ? commandId : "";
    ack["ok"] = false;
    ack["error"] = error ? error : "invalid_command";
    if (detail && *detail) ack["detail"] = detail;
    String payload;
    serializeJson(ack, payload);
    mqttClient.publishStatus(payload.c_str());
    StatusLed::triggerMessageFlash();
}

#ifdef FORGEKEY_LOCK
// Publish a cmd_ack for a lock verb. Includes command_id when the caller
// (OMS) provided one so the backend's structured ack path can match the row.
static void publishLockCmdAck(const char* cmd, const char* commandId,
                              const char* state, const char* error) {
    StaticJsonDocument<192> ack;
    ack["cmd_ack"] = cmd ? cmd : "";
    ack["command_id"] = commandId ? commandId : "";
    if (state && *state) ack["state"] = state;
    if (error && *error) ack["error"] = error;
    String payload;
    serializeJson(ack, payload);
    mqttClient.publishStatus(payload.c_str());
}
#endif

static void onCommandMessage(const char* topic, const uint8_t* payload, unsigned int length) {
    StaticJsonDocument<1536> doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
        debugPrintf("WARN", "CMD", "parse error: %s", err.c_str());
        publishCommandRejectAck("", nullptr, "parse_error", err.c_str());
        return;
    }
    const char* cmd = doc["cmd"] | "";
    const char* commandId = doc["command_id"] | "";
    debugPrintf("INFO", "CMD", "rx topic=%s cmd=%s len=%u",
                topic ? topic : "(null)",
                cmd[0] ? cmd : "(missing)",
                length);

    CommandValidation::Result validation = CommandValidation::validate(doc.as<JsonVariantConst>(), macAddress);
    if (!validation.ok) {
        publishCommandRejectAck(cmd, commandId, CommandValidation::canonicalError(validation));
        debugPrintf("WARN", "CMD", "%s rejected: %s",
                    cmd[0] ? cmd : "(missing)", CommandValidation::canonicalError(validation));
        return;
    }
    CommandValidation::rememberAccepted(validation);
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
        { StaticJsonDocument<96> ack; ack["cmd_ack"]="blink"; ack["command_id"]=commandId; ack["ok"]=true; String j; serializeJson(ack,j); mqttClient.publishStatus(j.c_str()); }
        return;
    }
    if (strcmp(cmd, "identify") == 0) {
        // Optional "duration_s": seconds before auto-clearing. 0/missing =
        // IDENTIFY_DEFAULT_DURATION_S. The auto-clear publishes its own
        // {"blink":"off"} echo from loop().
        unsigned long durationS = doc["duration_s"] | (unsigned long)IDENTIFY_DEFAULT_DURATION_S;
        unsigned long durationMs = durationS * 1000UL;
        bool wasOff = StatusLed::setBlinkOverrideTimed(durationMs);
        if (wasOff) mqttClient.publishBlinkStatus(true);
        StaticJsonDocument<128> ack;
        ack["cmd_ack"] = "identify";
        ack["command_id"] = commandId;
        ack["duration_s"] = durationS;
        String ackJson;
        serializeJson(ack, ackJson);
        mqttClient.publishStatus(ackJson.c_str());
        StatusLed::triggerMessageFlash();
        debugPrintf("INFO", "CMD", "identify -> %lus (was_off=%d)",
                    durationS, (int)wasOff);
        return;
    }
    if (strcmp(cmd, "status") == 0 || strcmp(cmd, "ping") == 0) {
        publishStatusSnapshot(cmd, commandId);
        debugPrintf("INFO", "CMD", "%s -> status snapshot", cmd);
        return;
    }
    if (strcmp(cmd, "capture") == 0 || strcmp(cmd, "capture_photo") == 0) {
#ifdef FORGEKEY_LOCK
        publishCommandRejectAck("capture", commandId, "capability_unsupported");
        debugPrint("WARN", "CMD", "capture rejected: capability unsupported on this build");
        return;
#elif !defined(FORGEKEY_TEMPERATURE_SENSOR) && !defined(FORGEKEY_EPAPER)
        if (PeopleCounter::isActive() && PeopleCounter::requestOneShotCapture()) {
            { StaticJsonDocument<128> ack; ack["cmd_ack"]="capture"; ack["command_id"]=commandId; ack["queued"]=true; String j; serializeJson(ack,j); mqttClient.publishStatus(j.c_str()); }
            StatusLed::triggerMessageFlash();
            debugPrint("INFO", "CMD", "capture queued");
        } else {
            publishCommandRejectAck("capture", commandId, "camera_unavailable");
            debugPrint("WARN", "CMD", "capture rejected: camera unavailable");
        }
#else
        publishCommandRejectAck("capture", commandId, "capability_unsupported");
        debugPrint("WARN", "CMD", "capture rejected: capability unsupported on this build");
#endif
        return;
    }
    if (strcmp(cmd, "restart") == 0) {
        // Ack first so the operator UI can register the device acknowledged
        // the command, then briefly delay to flush the publish through the
        // MQTT socket before yanking the chip.
        { StaticJsonDocument<128> ack; ack["cmd_ack"]="restart"; ack["command_id"]=commandId; ack["in_ms"]=1000; String j; serializeJson(ack,j); mqttClient.publishStatus(j.c_str()); }
        StatusLed::triggerMessageFlash();
        debugPrint("INFO", "CMD", "restart in 1000ms");
        // Drive the MQTT loop a few times so PubSubClient actually pushes
        // the publish out before vTaskDelay parks us.
        for (int i = 0; i < 10; ++i) {
            mqttClient.loop();
            delay(20);
        }
        delay(800);
        ESP.restart();
        return;  // unreachable
    }
#ifdef FORGEKEY_LOCK
    if (strcmp(cmd, "unlock") == 0) {
        const char* jwtField = doc["jwt"] | "";
        const char* commandId = doc["command_id"] | "";
        if (!jwtField || strlen(jwtField) == 0) {
            publishLockCmdAck("unlock", commandId, nullptr, "missing_jwt");
            debugPrint("WARN", "CMD", "unlock rejected: missing jwt field");
        } else if (LockCapability::handleUnlockCommand(jwtField, 0)) {
            publishLockCmdAck("unlock", commandId, "unlocked", nullptr);
            StatusLed::triggerMessageFlash();
            debugPrint("INFO", "CMD", "unlock accepted");
        } else {
            publishLockCmdAck("unlock", commandId, nullptr, "invalid_token");
            debugPrint("WARN", "CMD", "unlock rejected: invalid jwt");
        }
        return;
    }
    if (strcmp(cmd, "lockout") == 0 ||
        strcmp(cmd, "clear_lockout") == 0 ||
        strcmp(cmd, "init_ack") == 0) {
        const char* commandId = doc["command_id"] | "";
        publishLockCmdAck(cmd, commandId, nullptr, "not_implemented");
        debugPrintf("WARN", "CMD", "%s not implemented on this firmware build", cmd);
        return;
    }
#endif
#ifndef FORGEKEY_LOCK
#ifndef FORGEKEY_DISABLE_BLE_SCANNER
    if (strcmp(cmd, "ble_scan") == 0) {
        // Trigger an immediate BLE scan. The scanner capability will pick
        // up the request on its next tick and run a scan for BLE_SCAN_DURATION_S.
        { StaticJsonDocument<128> ack; ack["cmd_ack"]="ble_scan"; ack["command_id"]=commandId; ack["queued"]=true; String j; serializeJson(ack,j); mqttClient.publishStatus(j.c_str()); }
        StatusLed::triggerMessageFlash();
        debugPrint("INFO", "CMD", "ble_scan triggered");
        return;
    }
    if (strcmp(cmd, "ble_scan_stop") == 0) {
        { StaticJsonDocument<96> ack; ack["cmd_ack"]="ble_scan_stop"; ack["command_id"]=commandId; String j; serializeJson(ack,j); mqttClient.publishStatus(j.c_str()); }
        StatusLed::triggerMessageFlash();
        debugPrint("INFO", "CMD", "ble_scan_stop requested");
        return;
    }
#endif
#ifndef FORGEKEY_DISABLE_BLE_BEACON
    if (strcmp(cmd, "beacon") == 0) {
        const char* action = doc["action"] | "";
        if (strcmp(action, "start") == 0) {
            BleBeacon::setContinuous(true);
            { StaticJsonDocument<128> ack; ack["cmd_ack"]="beacon"; ack["command_id"]=commandId; ack["action"]="start"; String j; serializeJson(ack,j); mqttClient.publishStatus(j.c_str()); }
            debugPrint("INFO", "CMD", "beacon continuous ON");
        } else if (strcmp(action, "stop") == 0) {
            BleBeacon::setContinuous(false);
            { StaticJsonDocument<128> ack; ack["cmd_ack"]="beacon"; ack["command_id"]=commandId; ack["action"]="stop"; String j; serializeJson(ack,j); mqttClient.publishStatus(j.c_str()); }
            debugPrint("INFO", "CMD", "beacon continuous OFF");
        } else {
            publishCommandRejectAck("beacon", commandId, "missing_action");
            debugPrint("WARN", "CMD", "beacon: unknown action");
        }
        StatusLed::triggerMessageFlash();
        return;
    }
#endif
#ifndef FORGEKEY_DISABLE_BLE_EQUIPMENT
    if (strcmp(cmd, "equipment_list") == 0) {
        StaticJsonDocument<512> doc2;
        doc2["cmd_ack"] = "equipment_list";
        doc2["command_id"] = commandId;
        doc2["count"] = BleEquipment::getTagCount();
        JsonArray tags = doc2.createNestedArray("tags");
        for (int i = 0; i < BleEquipment::getTagCount(); i++) {
            const auto* t = BleEquipment::getTag(i);
            if (!t || !t->active) continue;
            JsonObject entry = tags.createNestedObject();
            entry["name"] = t->name;
            if (t->mac[0]) entry["mac"] = t->mac;
            if (t->ibeacon_uuid[0]) entry["ibeacon_uuid"] = t->ibeacon_uuid;
            entry["type"] = t->type;
            entry["zone"] = t->zone;
        }
        String json;
        serializeJson(doc2, json);
        mqttClient.publishStatus(json.c_str());
        StatusLed::triggerMessageFlash();
        return;
    }
    if (strcmp(cmd, "equipment_set") == 0) {
        // Tags are set via the config topic; this is a no-op ack.
        { StaticJsonDocument<128> ack; ack["cmd_ack"]="equipment_set"; ack["command_id"]=commandId; ack["note"]="use_config_topic"; String j; serializeJson(ack,j); mqttClient.publishStatus(j.c_str()); }
        StatusLed::triggerMessageFlash();
        debugPrint("INFO", "CMD", "equipment_set: configure via forgekey/<mac>/config");
        return;
    }
#endif
    if (strcmp(cmd, "forget_ble") == 0) {
        // Clear BLE peers and equipment tags from NVS.
        nvs_handle handle;
        if (nvs_open("forgekey_ble", NVS_READWRITE, &handle) == ESP_OK) {
            nvs_erase_key(handle, "peers");
            nvs_erase_key(handle, "tags");
            nvs_commit(handle);
            nvs_close(handle);
        }
        if (nvs_open("forgekey_relay", NVS_READWRITE, &handle) == ESP_OK) {
            nvs_erase_key(handle, "queue");
            nvs_commit(handle);
            nvs_close(handle);
        }

        // Also clear in-memory state for capabilities that track it
#ifndef FORGEKEY_DISABLE_BLE_SCANNER
        // Scanner: no persistent in-memory state to clear
#endif
#ifndef FORGEKEY_DISABLE_BLE_EQUIPMENT
        BleEquipment::clearTags();
#endif

        { StaticJsonDocument<96> ack; ack["cmd_ack"]="forget_ble"; ack["command_id"]=commandId; String j; serializeJson(ack,j); mqttClient.publishStatus(j.c_str()); }
        StatusLed::triggerMessageFlash();
        debugPrint("INFO", "CMD", "BLE data cleared from NVS");
        return;
    }
#endif
    debugPrintf("WARN", "CMD", "unknown cmd: '%s'", cmd);
    publishUnknownCommandAck(cmd, commandId);
}


// Single dispatcher for the shared forgekey/<mac>/config topic. The two
// payload shapes coexist:
//   {"cmd": "forget_wifi"}                     -> clear WiFi creds, reboot
//   {"cmd": "set_equipment", "tags": [...]}   -> configure equipment tags
//   {"cmd": "set_ble", "enabled": true}       -> enable/disable BLE
//   {"cmd": "set_wifi", "wifi": {"profiles": [...]}} -> two-phase WiFi desired-state
//   {"provisioning_token": "...", "valid_after": "..."}  -> token rotation
static void onConfigMessage(const char* topic, const uint8_t* payload, unsigned int length) {
    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, payload, length);
    if (!err) {
        const char* cmd = doc["cmd"] | "";
        const char* commandId = doc["command_id"] | "";
        if (strcmp(cmd, "forget_wifi") == 0) {
            debugPrint("INFO", "CFG", "forget_wifi command received");
            WifiSetup::forgetAndRestart();  // does not return
            return;
        }
        if (strcmp(cmd, "set_wifi") == 0) {
            String detail;
            bool ok = wifi_desired_state::apply(
                doc.as<JsonVariantConst>(),
                [](unsigned long timeoutMs) { return mqttClient.probeReachability(timeoutMs); },
                detail);
            StaticJsonDocument<192> ack;
            ack["cmd_ack"] = cmd;
            ack["command_id"] = commandId;
            ack["ok"] = ok;
            ack["detail"] = detail;
            String json;
            serializeJson(ack, json);
            mqttClient.publishStatus(json.c_str());
            StatusLed::triggerMessageFlash();
            debugPrintf(ok ? "INFO" : "WARN", "CFG", "wifi desired-state: %s", detail.c_str());
            return;
        }
        if (strcmp(cmd, "desired_state") == 0) {
            String wifiDetail;
            bool wifiOk = true;
            if (!doc["wifi"].isNull()) {
                wifiOk = wifi_desired_state::apply(
                    doc.as<JsonVariantConst>(),
                    [](unsigned long timeoutMs) { return mqttClient.probeReachability(timeoutMs); },
                    wifiDetail);
            } else {
                wifiDetail = "not_requested";
            }

            String bleDetail;
            bool bleOk = true;
            if (!doc["ble"].isNull()) {
                bleOk = ble_desired_state::apply(doc.as<JsonVariantConst>(), bleDetail);
#ifndef FORGEKEY_DISABLE_BLE_SCANNER
                BleScanner::setEnabled(ble_desired_state::current().scannerEnabled);
#endif
#ifndef FORGEKEY_DISABLE_BLE_BEACON
                BleBeacon::setEnabled(ble_desired_state::current().beaconEnabled);
#endif
#ifndef FORGEKEY_DISABLE_BLE_RELAY
                BleRelay::setEnabled(ble_desired_state::current().relayEnabled);
#endif
#ifndef FORGEKEY_DISABLE_BLE_EQUIPMENT
                BleEquipment::setEnabled(ble_desired_state::current().equipmentEnabled);
#endif
            } else {
                bleDetail = "not_requested";
            }

            StaticJsonDocument<256> ack;
            ack["cmd_ack"] = "desired_state";
            ack["command_id"] = commandId;
            ack["ok"] = wifiOk && bleOk;
            ack["wifi_detail"] = wifiDetail;
            ack["ble_detail"] = bleDetail;
            String json;
            serializeJson(ack, json);
            mqttClient.publishStatus(json.c_str());
            StatusLed::triggerMessageFlash();
            return;
        }
#ifndef FORGEKEY_LOCK
        if (strcmp(cmd, "set_ble") == 0) {
            String detail;
            bool ok = ble_desired_state::apply(doc.as<JsonVariantConst>(), detail);
            const ble_desired_state::BleConfig& cfg = ble_desired_state::current();

#ifndef FORGEKEY_DISABLE_BLE_SCANNER
            BleScanner::setEnabled(cfg.scannerEnabled);
#endif
#ifndef FORGEKEY_DISABLE_BLE_BEACON
            BleBeacon::setEnabled(cfg.beaconEnabled);
#endif
#ifndef FORGEKEY_DISABLE_BLE_RELAY
            BleRelay::setEnabled(cfg.relayEnabled);
#endif
#ifndef FORGEKEY_DISABLE_BLE_EQUIPMENT
            BleEquipment::setEnabled(cfg.equipmentEnabled);
#endif

            StaticJsonDocument<512> ack;
            ack["cmd_ack"] = "set_ble";
            ack["command_id"] = commandId;
            ack["ok"] = ok;
            ack["detail"] = detail;
            JsonObject ble = ack.createNestedObject("ble");
            ble["scanner"] = cfg.scannerEnabled;
            ble["beacon"] = cfg.beaconEnabled;
            ble["relay"] = cfg.relayEnabled;
            ble["equipment"] = cfg.equipmentEnabled;
            ble["scan_interval_ms"] = cfg.scanIntervalMs;
            ble["scan_duration_s"] = cfg.scanDurationS;
            ble["rssi_threshold"] = cfg.rssiThreshold;
            ble["raw_mac_enabled"] = cfg.rawMacEnabled;
            ble["identity_mode"] = cfg.identityMode;
            ble["site_namespace"] = cfg.siteNamespace;
            String json;
            serializeJson(ack, json);
            mqttClient.publishStatus(json.c_str());
            StatusLed::triggerMessageFlash();
            debugPrintf(ok ? "INFO" : "WARN", "CFG", "BLE desired-state: %s", detail.c_str());
            return;
        }
#ifndef FORGEKEY_DISABLE_BLE_EQUIPMENT
        if (strcmp(cmd, "set_equipment") == 0) {
            if (BleEquipment::setTagsFromJson(payload, length)) {
                { StaticJsonDocument<96> ack; ack["cmd_ack"]="set_equipment"; ack["command_id"]=commandId; ack["ok"]=true; String j; serializeJson(ack,j); mqttClient.publishStatus(j.c_str()); }
                StatusLed::triggerMessageFlash();
                debugPrint("INFO", "CFG", "equipment tags updated");
            } else {
                { StaticJsonDocument<96> ack; ack["cmd_ack"]="set_equipment"; ack["command_id"]=commandId; ack["ok"]=false; String j; serializeJson(ack,j); mqttClient.publishStatus(j.c_str()); }
                debugPrint("WARN", "CFG", "set_equipment: parse error");
            }
            return;
        }
#endif
#endif
    }
    // Anything else (rotation payload, parse error, unknown cmd) falls
    // through to the credential-rotation handler.
    credential_rotation::onConfigMessage(topic, payload, length);
}

static void onFirmwareDispatch(const char* topic, const uint8_t* payload, unsigned int length) {
    static String lastRapidDispatchKey;

    debugPrintf("INFO", "OTA", "Dispatch on %s (%u bytes)", topic, length);
    OtaUpdater::Spec spec;
    if (!otaUpdater.parse(payload, length, spec)) {
        mqttClient.publishFirmwareStatus("failed", "", -1, "parse_error");
        return;
    }
    debugPrintf("INFO", "OTA", "Target version %s mandatory=%d",
                spec.version.c_str(), (int)spec.mandatory);

    if (spec.version == FORGEKEY_FIRMWARE_VERSION) {
        debugPrintf("INFO", "OTA", "Ignoring current firmware version %s",
                    spec.version.c_str());
        return;
    }

    String policyRejectReason;
    if (!otaUpdater.isPolicyAllowed(spec, policyRejectReason)) {
        debugPrintf("INFO", "OTA", "Policy rejected %s: %s",
                    spec.version.c_str(), policyRejectReason.c_str());
        mqttClient.publishFirmwareStatus("rejected", spec.version.c_str(), -1,
                                         policyRejectReason.c_str());
        return;
    }

    String rapidDispatchKey = spec.version + ":" + spec.sha256;
    if (rapidDispatchKey != lastRapidDispatchKey) {
        lastRapidDispatchKey = rapidDispatchKey;
        extendOtaRapidChecking("new firmware dispatch received");
    }

    mqttClient.publishFirmwareStatus("received", spec.version.c_str(), -1, nullptr);
#ifndef FORGEKEY_LOCK
#if !defined(FORGEKEY_TEMPERATURE_SENSOR) && !defined(FORGEKEY_EPAPER)
    if (!spec.mandatory) {
        // Best-effort: defer if a photo upload was very recent.
        if (millis() - photoUploader.lastUploadMs() < 5000) {
            debugPrint("INFO", "OTA", "Deferring non-mandatory update — busy");
            mqttClient.publishFirmwareStatus("deferred", spec.version.c_str(), -1, "device_busy");
            return;
        }
    }
#endif
#endif
    otaUpdater.apply(spec);  // does not return on success
}

// First-boot: capture an artifact (people-counter photo if camera detected,
// otherwise empty body) and POST to OMS, submitting a CSR and getting back
// our signed device identity plus broker policy.
// Returns true on success (credentials persisted).
static bool runProvisioning() {
    uint8_t* jpeg = nullptr;
    size_t jpegLen = 0;
    bool havePhoto = false;

#ifdef FORGEKEY_LOCK
    // Lock builds have no camera — enroll with empty photo body.
    debugPrint("INFO", "PROV", "Enrolling with OMS (no photo: lock build)");
#elif !defined(FORGEKEY_TEMPERATURE_SENSOR) && !defined(FORGEKEY_EPAPER)
    if (PeopleCounter::isActive() &&
        PeopleCounter::captureProvisioningPhoto(&jpeg, &jpegLen)) {
        havePhoto = true;
        debugPrintf("INFO", "PROV", "Enrolling with OMS (photo: %u bytes)",
                    (unsigned)jpegLen);
    } else {
        debugPrint("INFO", "PROV",
                   "Enrolling with OMS (no photo: people-counter capability not active)");
    }
#else
    // Temperature builds have no camera. The OMS enroll endpoint accepts a
    // zero-length photo part for non-imaging device kinds.
    debugPrint("INFO", "PROV", "Enrolling with OMS (no photo: temperature build)");
#endif

    bool ok = provisioning.enrollDevice(OMS_HOST, OMS_PORT, macAddress,
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
    // FIRMWARE_BUILD_TIMESTAMP is injected by scripts/build/version.py
    // as a bare numeric macro (unsigned long epoch seconds), NOT a
    // string literal — using `%s` here makes printf dereference the
    // integer as a pointer, which on ESP32-C3 lands on an unmapped
    // address and load-faults at boot. ESP32-S3 tolerated it by
    // accident due to looser bus-error handling.
    debugPrintf("INFO", "MAIN", "Build timestamp (epoch): %lu",
                (unsigned long)FIRMWARE_BUILD_TIMESTAMP);
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

    ble_desired_state::begin();

    // Capability detection runs before WiFi: pure hardware probes. Any
    // capability whose detect() returns true gets setup() called immediately
    // afterwards. From this point on the LED state machine is live.
    debugPrint("INFO", "MAIN", "Detecting capabilities...");
    CapabilityRegistry::detectAll();
    BoardManifest::checkActiveCapabilityPins(CapabilityRegistry::head());
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
    StatusLed::requestState(StatusLed::State::Provisioning);
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

    // NTP sync: ESP32 has no RTC, so wall-clock security decisions remain
    // disabled until ForgeKeyTime marks the UTC epoch as valid.
    debugPrint("INFO", "MAIN", "Syncing NTP time...");
    ForgeKeyTime::begin();
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        debugPrint("WARN", "MAIN", "NTP sync failed — TLS and wall-clock signed commands may fail");
    } else {
        ForgeKeyTime::tick();
        debugPrintf("INFO", "MAIN", "NTP synced: %04d-%02d-%02d %02d:%02d:%02d",
                    timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                    timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    }

    // DNS diagnostic: dump full DNS state before any provisioning/MQTT attempts
    debugPrint("INFO", "MAIN", "DNS diagnostic (pre-provisioning):");
    WifiSetup::debug_dns_state("pre-provisioning");

#if defined(FORGEKEY_EPAPER)
    // The ePaper device class uses a different identity model from the
    // MAC-bound MQTT devices: a UUID `display_id` in NVS (set by an
    // OMS-side EPaperDisplay row) and an HTTPS-only contract. Running
    // the MQTT-flavored provisioning + mTLS enrollment here would
    // (a) try to register a duplicate device row in OMS keyed on the
    // panel's MAC, and (b) blow the loopTask stack mid-enrollment as
    // observed in the bench logs ("stack overflow in task loopTask"
    // right after "First boot — enrolling with OMS"). Skip the whole
    // block — the capability registry already drove
    // EPaperPmCapability::setupFn() earlier in setup() which is all
    // this device class needs.
    debugPrint("INFO", "MAIN",
               "ePaper build: skipping provisioning/MQTT setup "
               "(display-id HTTPS OTA polling enabled)");
    otaUpdater.begin();
    debugPrint("INFO", "MAIN", "Setup complete. Starting main loop...");
    return;
#endif

    provisioning.begin();
    CommandValidation::begin();
    otaUpdater.begin();
    extendOtaRapidChecking("restart");
    otaUpdater.setStatusCallback([](const char* state,
                                    const char* version,
                                    int progress,
                                    const char* error) {
        mqttClient.publishFirmwareStatus(state, version, progress, error);
    });

    if (!provisioning.isProvisioned()) {
        debugPrint("INFO", "PROV", "First boot — enrolling with OMS");
        if (!runProvisioning()) {
            debugPrint("WARN", "PROV", "Enrollment failed; will retry on next boot");
        } else {
            debugPrint("INFO", "PROV", "Enrollment successful");
            StatusLed::triggerMessageFlash();
        }
    } else {
        debugPrint("INFO", "PROV", "Already provisioned");
    }

    StatusLed::requestState(StatusLed::State::Normal);

    DeviceCredentials creds = provisioning.credentials();
    // Resolve broker connection info: NVS (set by enrollment response) wins
    // over the compile-time fallback. Source is logged so a wrong host on a
    // device in the field is diagnosable from a single boot log.
    String   brokerHost;
    uint16_t brokerPort;
    bool     brokerUseTls;
    const char* brokerSource;
    bool hasNvsBroker = creds.mqttBrokerHost.length() && creds.mqttBrokerPort != 0;
    if (hasNvsBroker) {
        brokerHost   = creds.mqttBrokerHost;
        brokerPort   = creds.mqttBrokerPort;
        brokerUseTls = creds.mqttBrokerUseTls;
        brokerSource = "nvs";
    } else {
        brokerHost   = MQTT_BROKER_FALLBACK_HOST;
        brokerPort   = MQTT_BROKER_FALLBACK_PORT;
        brokerUseTls = (MQTT_BROKER_FALLBACK_USE_TLS != 0);
        brokerSource = "fallback";
    }
    debugPrintf("INFO", "MQTT",
                "broker host=%s port=%u use_tls=%d source=%s "
                "(nvs_host='%s' nvs_port=%u nvs_tls=%d)",
                brokerHost.c_str(), (unsigned)brokerPort,
                (int)brokerUseTls, brokerSource,
                creds.mqttBrokerHost.c_str(), (unsigned)creds.mqttBrokerPort,
                (int)creds.mqttBrokerUseTls);

    mqttClient.setTopicPrefix(macAddress.c_str());
    mqttClient.begin(brokerHost.c_str(), brokerPort,
                     creds.clientCertificatePem.c_str(),
                     creds.clientPrivateKeyPem.c_str(),
                     brokerUseTls);

    // Validate the stored pings topic against the OMS contract before
    // applying it. A topic from an older firmware (leading '/') or from a
    // bad enrollment response would otherwise silently override the
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
                        "ignoring and clearing creds to force re-enrollment on next boot",
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

#ifdef FORGEKEY_LOCK
    // Lock verbs (unlock / lockout / clear_lockout / init_ack) flow in on
    // the same forgekey/<mac>/command topic as operator commands. JWT
    // verification + state-machine dispatch happens in onCommandMessage.
    LockWeb::begin();
    debugPrint("INFO", "LOCK", "embedded web server started");
#endif

#ifndef FORGEKEY_LOCK
#if !defined(FORGEKEY_TEMPERATURE_SENSOR) && !defined(FORGEKEY_EPAPER)
    photoUploader.begin(OMS_HOST, OMS_PORT, macAddress);
    photoUploader.setClientIdentity(creds.clientCertificatePem,
                                    creds.clientPrivateKeyPem);
#endif
#endif

    debugPrint("INFO", "MAIN", "Setup complete. Starting main loop...");
}

void loop() {
#if defined(FORGEKEY_EPAPER)
    // ePaper devices don't use MQTT or the announce/heartbeat path —
    // the capability registry's tickAll() drives the wake-cycle (HTTP
    // fetch + battery POST + deep sleep) on its own. Skip the rest of
    // the loop body so we don't dereference mqttClient/photoUploader
    // members that the ePaper setup() never initialised.
    CapabilityRegistry::tickAll();
    return;
#endif

    static bool mqttWasConnected = false;
    static bool capabilitiesAnnounced = false;

    bool mqttConnected = mqttClient.isConnected();
    if (mqttConnected && !mqttWasConnected) {
        flushBufferedDebugLogs();
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
            StatusLed::triggerMessageFlash();
        }
    }

    tickOtaRapidChecking(mqttConnected);

    CapabilityRegistry::tickAll();

#ifdef FORGEKEY_LOCK
    // Tick the embedded web server (handles client connections)
    LockWeb::tick();
#endif

    // Identify-blink auto-expiry echo. status_led's tick() flips this when a
    // timed override deadline elapses; mirror it onto the OMS status topic
    // so the device-control panel reflects the LED's actual state.
    if (StatusLed::consumeBlinkOverrideExpired()) {
        mqttClient.publishBlinkStatus(false);
        debugPrint("INFO", "CMD", "identify expired -> blink off");
    }

    ForgeKeyTime::tick();
    WifiSetup::tickHealth();

    mqttClient.loop();
    delay(10);
}
