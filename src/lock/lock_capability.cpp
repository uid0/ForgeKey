#include "lock_capability.h"
#include "lock_device.h"
#include "lock_config.h"
#include "../capabilities/capability.h"
#include "../mqtt/mqtt_client.h"
#include <WiFi.h>
#include <ArduinoJson.h>

namespace LockCapability {

static bool g_active = false;
static bool g_mqttReady = false;
static String g_macAddress;

void begin() {
    g_active = true;
    g_mqttReady = false;
    g_macAddress = WiFi.macAddress();
    g_macAddress.replace(":", "");
    g_macAddress.toLowerCase();

    LockDevice::begin();
    Serial.printf("[LOCK/CAP] initialized: mac=%s\n", g_macAddress.c_str());
}

void tick() {
    if (!g_active) return;

    LockDevice::tick();

    // Once MQTT is connected, mark it ready
    if (mqttClient.isConnected()) {
        g_mqttReady = true;
    }

    // Telemetry publishing is handled by main.cpp on state change
}

bool handleUnlockCommand(const char* token, long timestamp) {
    if (!g_active) return false;
    return LockDevice::handleUnlockCommand(token, timestamp);
}

bool publishTelemetry() {
    if (!g_mqttReady) return false;

    auto tel = LockDevice::getTelemetry();
    auto trigger = LockDevice::getLastTrigger();

    const char* triggerStr = "unknown";
    switch (trigger) {
        case LockDevice::Trigger::JWT: triggerStr = "jwt"; break;
        case LockDevice::Trigger::MORTISE: triggerStr = "mortise"; break;
        case LockDevice::Trigger::AUTO_UNLOCK: triggerStr = "auto_unlock"; break;
        case LockDevice::Trigger::DOOR_CLOSE: triggerStr = "door_close"; break;
        case LockDevice::Trigger::ALARM_TIMEOUT: triggerStr = "alarm_timeout"; break;
        default: triggerStr = "unknown"; break;
    }

    StaticJsonDocument<256> doc;
    doc["mac"] = g_macAddress;
    doc["secure"] = tel.secure;
    doc["item_present"] = !tel.ir_broken;
    doc["uptime"] = tel.uptime_ms;
    doc["last_trigger"] = triggerStr;
    doc["state"] = LockDevice::stateName(LockDevice::getState());
    doc["reed_closed"] = tel.reed_closed;
    doc["latch_locked"] = tel.latch_locked;
    doc["ir_broken"] = tel.ir_broken;
    doc["mortise_active"] = tel.mortise_active;

    String json;
    serializeJson(doc, json);

    String topic = String("forgekey/") + g_macAddress + "/cabinet_lock/status";

    bool ok = mqttClient.publishStatus(json.c_str());
    if (ok) {
        Serial.printf("[LOCK/TEL] published: %s\n", json.c_str());
    } else {
        Serial.println("[LOCK/TEL] publish FAILED");
    }

    return ok;
}

const char* getStateName() {
    if (!g_active) return "DISABLED";
    return LockDevice::stateName(LockDevice::getState());
}

bool isActive() {
    return g_active;
}

}  // namespace LockCapability

// Capability registration
static bool lockDetect() {
    return true;  // lock capability is always active in this build
}

static void lockSetup() {
    LockCapability::begin();
}

static void lockTick() {
    LockCapability::tick();
}

REGISTER_CAPABILITY(lock_cap, "cabinet_lock",
                    lockDetect,
                    lockSetup,
                    lockTick,
                    "cabinet_lock/status")
