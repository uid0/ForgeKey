#include "power_relay.h"

#include "capabilities/capability.h"
#include "mqtt/mqtt_client.h"
#include "ota/ota_updater.h"
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
#ifndef FORGEKEY_POWER_RELAY_METER_ENABLED
#define FORGEKEY_POWER_RELAY_METER_ENABLED 1
#endif
#ifndef FORGEKEY_POWER_RELAY_METER_RX_PIN
#define FORGEKEY_POWER_RELAY_METER_RX_PIN 20
#endif
#ifndef FORGEKEY_POWER_RELAY_METER_TX_PIN
#define FORGEKEY_POWER_RELAY_METER_TX_PIN 21
#endif
#ifndef FORGEKEY_POWER_RELAY_METER_BAUD
#define FORGEKEY_POWER_RELAY_METER_BAUD 2400
#endif
#ifndef FORGEKEY_POWER_RELAY_METER_ADDRESS
#define FORGEKEY_POWER_RELAY_METER_ADDRESS 0
#endif
#ifndef FORGEKEY_POWER_RELAY_METER_LINE_FREQ_HZ
#define FORGEKEY_POWER_RELAY_METER_LINE_FREQ_HZ 60
#endif
#ifndef FORGEKEY_POWER_RELAY_METER_SAMPLE_INTERVAL_MS
#define FORGEKEY_POWER_RELAY_METER_SAMPLE_INTERVAL_MS 15000UL
#endif
#ifndef FORGEKEY_POWER_RELAY_USAGE_PUBLISH_INTERVAL_MS
#define FORGEKEY_POWER_RELAY_USAGE_PUBLISH_INTERVAL_MS 60000UL
#endif
#ifndef FORGEKEY_POWER_RELAY_BL0942_CURRENT_REF
#define FORGEKEY_POWER_RELAY_BL0942_CURRENT_REF 251065.6814f
#endif
#ifndef FORGEKEY_POWER_RELAY_BL0942_VOLTAGE_REF
#define FORGEKEY_POWER_RELAY_BL0942_VOLTAGE_REF 15883.34116f
#endif
#ifndef FORGEKEY_POWER_RELAY_BL0942_POWER_REF
#define FORGEKEY_POWER_RELAY_BL0942_POWER_REF 623.0270705f
#endif
#ifndef FORGEKEY_POWER_RELAY_BL0942_ENERGY_REF
#define FORGEKEY_POWER_RELAY_BL0942_ENERGY_REF 5347.484240f
#endif
#ifndef FORGEKEY_POWER_RELAY_BL0942_RESET_ON_BOOT
#define FORGEKEY_POWER_RELAY_BL0942_RESET_ON_BOOT 0
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
unsigned long g_lastUsagePublishMs = 0;

#if FORGEKEY_POWER_RELAY_METER_ENABLED
constexpr uint8_t kBl0942ReadCommand = 0x58;
constexpr uint8_t kBl0942FullPacket = 0xAA;
constexpr uint8_t kBl0942PacketHeader = 0x55;
constexpr uint8_t kBl0942WriteCommand = 0xA8;
constexpr uint8_t kBl0942RegMode = 0x19;
constexpr uint8_t kBl0942RegSoftReset = 0x1C;
constexpr uint8_t kBl0942RegUserWriteProtect = 0x1D;
constexpr uint32_t kBl0942ModeReserved = 0x03;
constexpr uint32_t kBl0942ModeCfEnable = 0x04;
constexpr uint32_t kBl0942ModeRmsUpdate800Ms = 0x08;
constexpr uint32_t kBl0942ModeAcFrequency60Hz = 0x20;
constexpr uint32_t kBl0942ModeCfCntAdd = 0x80;
constexpr uint32_t kBl0942SoftResetMagic = 0x5a5a5a;
constexpr uint32_t kBl0942UserWriteProtectMagic = 0x55;
constexpr size_t kBl0942PacketSize = 23;
constexpr unsigned long kBl0942PacketTimeoutMs = 300UL;

uint8_t g_meterBuffer[kBl0942PacketSize] = {0};
size_t g_meterBufferLen = 0;
unsigned long g_meterRxStartMs = 0;
unsigned long g_lastMeterRequestMs = 0;
unsigned long g_lastMeterSampleMs = 0;

struct MeterState {
    bool valid = false;
    uint32_t samples = 0;
    uint32_t checksumErrors = 0;
    uint32_t junkBytes = 0;
    uint32_t partialTimeouts = 0;
    const char* lastError = "";
    uint32_t rawCurrent = 0;
    uint32_t rawVoltage = 0;
    uint32_t rawFastCurrent = 0;
    int32_t rawPower = 0;
    uint32_t rawEnergyCount = 0;
    uint16_t rawFrequency = 0;
    uint8_t status = 0;
    float voltageV = 0.0f;
    float currentA = 0.0f;
    float powerW = 0.0f;
    float energyKwh = 0.0f;
    float frequencyHz = 0.0f;
};

MeterState g_meter;
#endif

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

void appendChannelsJson(String& out) {
    out += "\"channels\":[";
    for (uint8_t i = 0; i < kChannelCount; ++i) {
        if (i) out += ",";
        out += "{\"channel\":";
        out += String(i + 1);
        out += ",\"on\":";
        out += g_states[i] ? "true" : "false";
        out += "}";
    }
    out += "]";
}

void appendFloatJson(String& out, float value, uint8_t decimals) {
    out += String(value, (unsigned int)decimals);
}

#if FORGEKEY_POWER_RELAY_METER_ENABLED
uint32_t readU24(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

int32_t readS24(const uint8_t* p) {
    uint32_t value = readU24(p);
    if (value & 0x800000UL) value |= 0xFF000000UL;
    return (int32_t)value;
}

uint16_t readU16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint8_t meterAddress() {
    return (uint8_t)(FORGEKEY_POWER_RELAY_METER_ADDRESS & 0x03);
}

void setMeterError(const char* error) {
    g_meter.lastError = error ? error : "";
}

void writeBl0942Register(uint8_t reg, uint32_t value) {
    uint8_t pkt[6];
    pkt[0] = kBl0942WriteCommand | meterAddress();
    pkt[1] = reg;
    pkt[2] = value & 0xff;
    pkt[3] = (value >> 8) & 0xff;
    pkt[4] = (value >> 16) & 0xff;
    pkt[5] = (pkt[0] + pkt[1] + pkt[2] + pkt[3] + pkt[4]) ^ 0xff;
    Serial1.write(pkt, sizeof(pkt));
    Serial1.flush();
    delay(1);
}

void setupBl0942() {
    Serial1.begin(FORGEKEY_POWER_RELAY_METER_BAUD, SERIAL_8N1,
                  FORGEKEY_POWER_RELAY_METER_RX_PIN,
                  FORGEKEY_POWER_RELAY_METER_TX_PIN);

    writeBl0942Register(kBl0942RegUserWriteProtect, kBl0942UserWriteProtectMagic);
#if FORGEKEY_POWER_RELAY_BL0942_RESET_ON_BOOT
    writeBl0942Register(kBl0942RegSoftReset, kBl0942SoftResetMagic);
#endif
    uint32_t mode = kBl0942ModeReserved | kBl0942ModeCfEnable |
                    kBl0942ModeCfCntAdd | kBl0942ModeRmsUpdate800Ms;
    if (FORGEKEY_POWER_RELAY_METER_LINE_FREQ_HZ == 60) {
        mode |= kBl0942ModeAcFrequency60Hz;
    }
    writeBl0942Register(kBl0942RegMode, mode);
    writeBl0942Register(kBl0942RegUserWriteProtect, 0);

    while (Serial1.available() > 0) (void)Serial1.read();
    Serial.printf("[POWER_RELAY] BL0942 meter uart rx=%d tx=%d baud=%lu line_hz=%d\n",
                  FORGEKEY_POWER_RELAY_METER_RX_PIN,
                  FORGEKEY_POWER_RELAY_METER_TX_PIN,
                  (unsigned long)FORGEKEY_POWER_RELAY_METER_BAUD,
                  FORGEKEY_POWER_RELAY_METER_LINE_FREQ_HZ);
}

void requestBl0942Sample(unsigned long now) {
    if (g_lastMeterRequestMs != 0 &&
        now - g_lastMeterRequestMs < FORGEKEY_POWER_RELAY_METER_SAMPLE_INTERVAL_MS) {
        return;
    }
    g_lastMeterRequestMs = now;
    uint8_t cmd[2] = {static_cast<uint8_t>(kBl0942ReadCommand | meterAddress()), kBl0942FullPacket};
    Serial1.write(cmd, sizeof(cmd));
    Serial1.flush();
}

bool validateBl0942Checksum(const uint8_t* packet) {
    uint8_t checksum = kBl0942ReadCommand | meterAddress();
    for (size_t i = 0; i < kBl0942PacketSize - 1; ++i) {
        checksum += packet[i];
    }
    checksum ^= 0xff;
    return checksum == packet[kBl0942PacketSize - 1];
}

void parseBl0942Packet(const uint8_t* packet, unsigned long now) {
    if (packet[0] != kBl0942PacketHeader) {
        g_meter.junkBytes++;
        setMeterError("header_mismatch");
        return;
    }
    if (!validateBl0942Checksum(packet)) {
        g_meter.checksumErrors++;
        setMeterError("checksum");
        return;
    }

    g_meter.rawCurrent = readU24(packet + 1);
    g_meter.rawVoltage = readU24(packet + 4);
    g_meter.rawFastCurrent = readU24(packet + 7);
    g_meter.rawPower = readS24(packet + 10);
    g_meter.rawEnergyCount = readU24(packet + 13);
    g_meter.rawFrequency = readU16(packet + 16);
    g_meter.status = packet[19];

    g_meter.currentA = g_meter.rawCurrent / FORGEKEY_POWER_RELAY_BL0942_CURRENT_REF;
    g_meter.voltageV = g_meter.rawVoltage / FORGEKEY_POWER_RELAY_BL0942_VOLTAGE_REF;
    g_meter.powerW = g_meter.rawPower / FORGEKEY_POWER_RELAY_BL0942_POWER_REF;
    g_meter.energyKwh = g_meter.rawEnergyCount / FORGEKEY_POWER_RELAY_BL0942_ENERGY_REF;
    g_meter.frequencyHz = g_meter.rawFrequency != 0
                              ? 1000000.0f / (float)g_meter.rawFrequency
                              : 0.0f;
    g_meter.valid = true;
    g_meter.samples++;
    g_lastMeterSampleMs = now;
    setMeterError("");
}

void resetMeterBuffer(const char* error) {
    g_meterBufferLen = 0;
    g_meterRxStartMs = 0;
    if (error && *error) setMeterError(error);
}

void serviceMeterSerial(unsigned long now) {
    if (g_meterBufferLen > 0 && now - g_meterRxStartMs > kBl0942PacketTimeoutMs) {
        g_meter.partialTimeouts++;
        resetMeterBuffer("partial_timeout");
    }

    while (Serial1.available() > 0) {
        uint8_t b = (uint8_t)Serial1.read();
        if (g_meterBufferLen == 0 && b != kBl0942PacketHeader) {
            g_meter.junkBytes++;
            setMeterError("junk");
            continue;
        }
        if (g_meterBufferLen == 0) {
            g_meterRxStartMs = now;
        }
        g_meterBuffer[g_meterBufferLen++] = b;
        if (g_meterBufferLen == kBl0942PacketSize) {
            parseBl0942Packet(g_meterBuffer, now);
            resetMeterBuffer("");
        }
    }
}

void appendMeterJson(String& out) {
    out += "\"metering\":{\"chip\":\"BL0942\",\"enabled\":true";
    out += ",\"valid\":";
    out += g_meter.valid ? "true" : "false";
    out += ",\"sample_interval_ms\":";
    out += String((unsigned long)FORGEKEY_POWER_RELAY_METER_SAMPLE_INTERVAL_MS);
    out += ",\"publish_interval_ms\":";
    out += String((unsigned long)FORGEKEY_POWER_RELAY_USAGE_PUBLISH_INTERVAL_MS);
    out += ",\"samples\":";
    out += String((unsigned long)g_meter.samples);
    out += ",\"checksum_errors\":";
    out += String((unsigned long)g_meter.checksumErrors);
    out += ",\"junk_bytes\":";
    out += String((unsigned long)g_meter.junkBytes);
    out += ",\"partial_timeouts\":";
    out += String((unsigned long)g_meter.partialTimeouts);
    if (g_meter.lastError && *g_meter.lastError) {
        out += ",\"last_error\":\"";
        out += g_meter.lastError;
        out += "\"";
    }
    if (g_meter.valid) {
        out += ",\"sample_age_ms\":";
        out += String(millis() - g_lastMeterSampleMs);
        out += ",\"voltage_v\":";
        appendFloatJson(out, g_meter.voltageV, 1);
        out += ",\"current_a\":";
        appendFloatJson(out, g_meter.currentA, 3);
        out += ",\"power_w\":";
        appendFloatJson(out, g_meter.powerW, 1);
        out += ",\"energy_kwh\":";
        appendFloatJson(out, g_meter.energyKwh, 5);
        out += ",\"frequency_hz\":";
        if (g_meter.rawFrequency == 0) {
            out += "null";
        } else {
            appendFloatJson(out, g_meter.frequencyHz, 2);
        }
        out += ",\"raw\":{\"i_rms\":";
        out += String((unsigned long)g_meter.rawCurrent);
        out += ",\"v_rms\":";
        out += String((unsigned long)g_meter.rawVoltage);
        out += ",\"i_fast_rms\":";
        out += String((unsigned long)g_meter.rawFastCurrent);
        out += ",\"watt\":";
        out += String((long)g_meter.rawPower);
        out += ",\"cf_cnt\":";
        out += String((unsigned long)g_meter.rawEnergyCount);
        out += ",\"frequency\":";
        out += String((unsigned)g_meter.rawFrequency);
        out += ",\"status\":";
        out += String((unsigned)g_meter.status);
        out += "}";
    }
    out += "}";
}
#else
void setupBl0942() {}
void requestBl0942Sample(unsigned long) {}
void serviceMeterSerial(unsigned long) {}
void appendMeterJson(String& out) {
    out += "\"metering\":{\"enabled\":false,\"valid\":false}";
}
#endif

bool publishUsageStats(const char* reason, const char* commandId = nullptr) {
    if (!g_ready || !mqttClient.isConnected()) return false;
    String payload;
    payload.reserve(640);
    payload += "{\"power_relay\":";
    appendUsageJson(payload);
    if (reason && *reason) {
        payload += ",\"reason\":\"";
        payload += reason;
        payload += "\"";
    }
    if (commandId && *commandId) {
        payload += ",\"command_id\":\"";
        payload += commandId;
        payload += "\"";
    }
    payload += "}";
    bool ok = mqttClient.publishStatus(payload.c_str());
    if (ok) otaUpdater.markStableIfPending();
    return ok;
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
    setupBl0942();
    Serial.printf("[POWER_RELAY] ready ch1=%d ch2=%d pulse_ms=%u\n",
                  (int)g_states[0], (int)g_states[1], (unsigned)FORGEKEY_POWER_RELAY_PULSE_MS);
}

void tickFn() {
    unsigned long now = millis();
    requestBl0942Sample(now);
    serviceMeterSerial(now);
    if (g_lastUsagePublishMs == 0 ||
        now - g_lastUsagePublishMs >= FORGEKEY_POWER_RELAY_USAGE_PUBLISH_INTERVAL_MS) {
        if (publishUsageStats("periodic")) {
            g_lastUsagePublishMs = now;
            Serial.println("[POWER_RELAY] published usage stats");
        }
    }
}
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
    appendUsageJson(out);
}

void appendUsageJson(String& out) {
    out += "{";
    appendChannelsJson(out);
    out += ",\"ready\":";
    out += g_ready ? "true" : "false";
    out += ",";
    appendMeterJson(out);
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
    if (ackJson.length() > 0 && ackJson[ackJson.length() - 1] == '}') {
        ackJson.remove(ackJson.length() - 1);
        ackJson += ",\"power_relay\":";
        appendUsageJson(ackJson);
        ackJson += "}";
    }
    return ok;
}

REGISTER_CAPABILITY(power_relay, "power_relay", detectFn, setupFn, tickFn, "status");

}  // namespace PowerRelay
