#include "power_relay.h"

#include "capabilities/capability.h"
#include <nvs.h>
#include <nvs_flash.h>

#ifndef FORGEKEY_POWER_RELAY_PULSE_MS
#define FORGEKEY_POWER_RELAY_PULSE_MS 100
#endif
#ifndef FORGEKEY_POWER_RELAY_ACTIVE_LEVEL
#define FORGEKEY_POWER_RELAY_ACTIVE_LEVEL HIGH
#endif
#ifndef FORGEKEY_POWER_RELAY_CH1_SET_PIN
#define FORGEKEY_POWER_RELAY_CH1_SET_PIN 5
#endif
#ifndef FORGEKEY_POWER_RELAY_CH1_RESET_PIN
#define FORGEKEY_POWER_RELAY_CH1_RESET_PIN 4
#endif
#ifndef FORGEKEY_POWER_RELAY_CH2_SET_PIN
#define FORGEKEY_POWER_RELAY_CH2_SET_PIN 7
#endif
#ifndef FORGEKEY_POWER_RELAY_CH2_RESET_PIN
#define FORGEKEY_POWER_RELAY_CH2_RESET_PIN 6
#endif

namespace PowerRelay {
namespace {
constexpr const char* kNvsNamespace = "powerrelay";
constexpr const char* kNvsStatesKey = "states";
constexpr uint8_t kInactiveLevel = FORGEKEY_POWER_RELAY_ACTIVE_LEVEL == HIGH ? LOW : HIGH;
const uint8_t kSetPins[kChannelCount] = {FORGEKEY_POWER_RELAY_CH1_SET_PIN, FORGEKEY_POWER_RELAY_CH2_SET_PIN};
const uint8_t kResetPins[kChannelCount] = {FORGEKEY_POWER_RELAY_CH1_RESET_PIN, FORGEKEY_POWER_RELAY_CH2_RESET_PIN};
bool g_states[kChannelCount] = {false, false};
bool g_ready = false;

void writeNvs() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    uint8_t mask = 0;
    for (uint8_t i = 0; i < kChannelCount; ++i) {
        if (g_states[i]) mask |= (1U << i);
    }
    nvs_set_u8(handle, kNvsStatesKey, mask);
    nvs_commit(handle);
    nvs_close(handle);
}

void readNvs() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t mask = 0;
    if (nvs_get_u8(handle, kNvsStatesKey, &mask) == ESP_OK) {
        for (uint8_t i = 0; i < kChannelCount; ++i) g_states[i] = (mask & (1U << i)) != 0;
    }
    nvs_close(handle);
}

void pulse(uint8_t pin) {
    digitalWrite(pin, FORGEKEY_POWER_RELAY_ACTIVE_LEVEL);
    delay(FORGEKEY_POWER_RELAY_PULSE_MS);
    digitalWrite(pin, kInactiveLevel);
}

bool detectFn() {
#ifdef FORGEKEY_POWER_RELAY
    return true;
#else
    return false;
#endif
}

void setupFn() {
    for (uint8_t i = 0; i < kChannelCount; ++i) {
        pinMode(kSetPins[i], OUTPUT);
        pinMode(kResetPins[i], OUTPUT);
        digitalWrite(kSetPins[i], kInactiveLevel);
        digitalWrite(kResetPins[i], kInactiveLevel);
    }
    readNvs();
    g_ready = true;
    Serial.printf("[POWER_RELAY] ready ch1=%d ch2=%d pulse_ms=%u\n",
                  (int)g_states[0], (int)g_states[1], (unsigned)FORGEKEY_POWER_RELAY_PULSE_MS);
}

void tickFn() {}
}  // namespace

bool channelState(uint8_t channel) {
    if (channel < 1 || channel > kChannelCount) return false;
    return g_states[channel - 1];
}

bool setChannel(uint8_t channel, bool on, String& detail) {
    if (!g_ready) {
        detail = "power_relay_not_ready";
        return false;
    }
    if (channel < 1 || channel > kChannelCount) {
        detail = "invalid_channel";
        return false;
    }
    uint8_t idx = channel - 1;
    pulse(on ? kSetPins[idx] : kResetPins[idx]);
    g_states[idx] = on;
    writeNvs();
    detail = "ok";
    return true;
}

void appendHealthJson(String& out) {
    out += "{\"channels\":[";
    for (uint8_t i = 0; i < kChannelCount; ++i) {
        if (i) out += ",";
        out += "{\"channel\":";
        out += String(i + 1);
        out += ",\"on\":";
        out += g_states[i] ? "true" : "false";
        out += "}";
    }
    out += "],\"ready\":";
    out += g_ready ? "true" : "false";
    out += "}";
}

bool setFromCommand(JsonVariantConst doc, const char* commandId, String& ackJson) {
    uint8_t channel = 0;
    if (!doc["channel"].isNull()) channel = doc["channel"].as<uint8_t>();
    else if (!doc["relay"].isNull()) channel = doc["relay"].as<uint8_t>();
    bool requestedOn = false;
    if (!doc["on"].isNull()) {
        requestedOn = doc["on"].as<bool>();
    } else {
        const char* action = doc["action"] | "";
        if (strcmp(action, "on") == 0 || strcmp(action, "enable") == 0) requestedOn = true;
        else if (strcmp(action, "off") == 0 || strcmp(action, "disable") == 0) requestedOn = false;
        else if (strcmp(action, "toggle") == 0 && channel >= 1 && channel <= kChannelCount) requestedOn = !g_states[channel - 1];
        else channel = 0;
    }
    String detail;
    bool ok = setChannel(channel, requestedOn, detail);
    JsonDocument ack;
    ack["cmd_ack"] = "power_set";
    ack["command_id"] = commandId ? commandId : "";
    ack["ok"] = ok;
    ack["channel"] = channel;
    ack["on"] = ok ? requestedOn : false;
    ack["detail"] = detail;
    JsonArray states = ack["channels"].to<JsonArray>();
    for (uint8_t i = 0; i < kChannelCount; ++i) {
        JsonObject state = states.add<JsonObject>();
        state["channel"] = i + 1;
        state["on"] = g_states[i];
    }
    serializeJson(ack, ackJson);
    return ok;
}

REGISTER_CAPABILITY(power_relay, "power_relay", detectFn, setupFn, tickFn, "status");

}  // namespace PowerRelay
