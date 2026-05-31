#include "board_manifest.h"
#include "provisioning/device_config.h"
#include <string.h>

namespace BoardManifest {
namespace {

constexpr int kNoPin = -1;

const int S3_BOOTSTRAPS[] = {0, 3, 45, 46};
const int S3_ADC[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
const int C3_BOOTSTRAPS[] = {2, 8, 9};
const int C3_ADC[] = {0, 1, 2, 3, 4};

#ifndef FORGEKEY_STATUS_LED_PIN
#if defined(FORGEKEY_EPAPER)
#define FORGEKEY_STATUS_LED_PIN (-1)
#else
#define FORGEKEY_STATUS_LED_PIN 21
#endif
#endif

#ifndef FORGEKEY_STATUS_LED_ACTIVE_LOW
#define FORGEKEY_STATUS_LED_ACTIVE_LOW 1
#endif

#ifndef FORGEKEY_BUTTON_PIN
#define FORGEKEY_BUTTON_PIN (-1)
#endif

#ifndef FORGEKEY_MMWAVE_RX_PIN
#define FORGEKEY_MMWAVE_RX_PIN (-1)
#endif

#ifndef FORGEKEY_MMWAVE_TX_PIN
#define FORGEKEY_MMWAVE_TX_PIN (-1)
#endif

#ifndef FORGEKEY_MMWAVE_BAUD
#define FORGEKEY_MMWAVE_BAUD 256000UL
#endif

#if defined(FORGEKEY_EPAPER)
const PinClaim CURRENT_PINS[] = {
    {2, "epaper_pm", "epaper_rst", PullExpectation::Driven, true, true},
    {3, "epaper_pm", "epaper_cs", PullExpectation::Driven, true, false},
    {4, "epaper_pm", "epaper_busy", PullExpectation::Driven, false, false},
    {5, "epaper_pm", "epaper_dc", PullExpectation::Driven, true, false},
    {8, "epaper_pm", "spi_sck", PullExpectation::Driven, true, true},
    {9, "epaper_pm", "spi_miso", PullExpectation::Driven, false, true},
    {10, "epaper_pm", "spi_mosi", PullExpectation::Driven, true, false},
};
const BusManifest CURRENT_BUSES[] = {
    {"spi0", "epaper_pm", kNoPin, kNoPin, 8, 9, 10, kNoPin, kNoPin},
};
const Board CURRENT_BOARD = {
    "seeed_xiao_epaper",
    "Seeed XIAO ESP32-C3 ePaper",
    CURRENT_PINS,
    sizeof(CURRENT_PINS) / sizeof(CURRENT_PINS[0]),
    C3_BOOTSTRAPS,
    sizeof(C3_BOOTSTRAPS) / sizeof(C3_BOOTSTRAPS[0]),
    C3_ADC,
    sizeof(C3_ADC) / sizeof(C3_ADC[0]),
    CURRENT_BUSES,
    sizeof(CURRENT_BUSES) / sizeof(CURRENT_BUSES[0]),
};
#else
const PinClaim CURRENT_PINS[] = {
    {10, "people_counter", "camera_xclk", PullExpectation::Driven, true, false},
    {13, "people_counter", "camera_pclk", PullExpectation::Driven, false, false},
    {38, "people_counter", "camera_vsync", PullExpectation::Driven, false, false},
    {47, "people_counter", "camera_href", PullExpectation::Driven, false, false},
    {15, "people_counter", "camera_d0", PullExpectation::Driven, false, false},
    {17, "people_counter", "camera_d1", PullExpectation::Driven, false, false},
    {18, "people_counter", "camera_d2", PullExpectation::Driven, false, false},
    {16, "people_counter", "camera_d3", PullExpectation::Driven, false, false},
    {14, "people_counter", "camera_d4", PullExpectation::Driven, false, false},
    {12, "people_counter", "camera_d5", PullExpectation::Driven, false, false},
    {11, "people_counter", "camera_d6", PullExpectation::Driven, false, false},
    {48, "people_counter", "camera_d7", PullExpectation::Driven, false, false},
    {40, "people_counter", "camera_sda", PullExpectation::Driven, true, false},
    {39, "people_counter", "camera_scl", PullExpectation::Driven, true, false},
    {FORGEKEY_STATUS_LED_PIN, "status_led", "onboard_led", PullExpectation::None, true, false},
#if defined(FORGEKEY_TEMPERATURE_SENSOR)
    {FORGEKEY_DHT_PIN, "temperature_sensor", "dht_data", PullExpectation::External, false, false},
#endif
#if FORGEKEY_BUTTON_PIN >= 0
    {FORGEKEY_BUTTON_PIN, "button", "button", PullExpectation::PullUp, false, false},
#endif
#if FORGEKEY_MMWAVE_RX_PIN >= 0
    {FORGEKEY_MMWAVE_RX_PIN, "mmwave_presence", "uart_rx", PullExpectation::None, false, false},
#endif
#if FORGEKEY_MMWAVE_TX_PIN >= 0
    {FORGEKEY_MMWAVE_TX_PIN, "mmwave_presence", "uart_tx", PullExpectation::None, true, false},
#endif
};
const BusManifest CURRENT_BUSES[] = {
    {"camera_sccb", "people_counter", 40, 39, kNoPin, kNoPin, kNoPin, kNoPin, kNoPin},
#if FORGEKEY_MMWAVE_RX_PIN >= 0 || FORGEKEY_MMWAVE_TX_PIN >= 0
    {"uart_mmwave", "mmwave_presence", kNoPin, kNoPin, kNoPin, kNoPin, kNoPin, FORGEKEY_MMWAVE_RX_PIN, FORGEKEY_MMWAVE_TX_PIN},
#endif
};
const Board CURRENT_BOARD = {
    "seeed_xiao_esp32s3",
    "Seeed XIAO ESP32-S3",
    CURRENT_PINS,
    sizeof(CURRENT_PINS) / sizeof(CURRENT_PINS[0]),
    S3_BOOTSTRAPS,
    sizeof(S3_BOOTSTRAPS) / sizeof(S3_BOOTSTRAPS[0]),
    S3_ADC,
    sizeof(S3_ADC) / sizeof(S3_ADC[0]),
    CURRENT_BUSES,
    sizeof(CURRENT_BUSES) / sizeof(CURRENT_BUSES[0]),
};
#endif

bool streq(const char* a, const char* b) {
    return a && b && strcmp(a, b) == 0;
}

bool pinInList(int pin, const int* pins, size_t count) {
    if (pin < 0) return false;
    for (size_t i = 0; i < count; ++i) {
        if (pins[i] == pin) return true;
    }
    return false;
}

bool ownerHasPin(const char* owner, int pin) {
    if (!owner || pin < 0) return false;
    const Board& b = current();
    for (size_t i = 0; i < b.pinCount; ++i) {
        if (b.pins[i].pin == pin && streq(b.pins[i].owner, owner)) return true;
    }
    return false;
}

bool capabilityHasAnyPin(const char* owner) {
    if (!owner) return false;
    const Board& b = current();
    for (size_t i = 0; i < b.pinCount; ++i) {
        if (b.pins[i].pin >= 0 && streq(b.pins[i].owner, owner)) return true;
    }
    return false;
}

void appendEscaped(String& payload, const char* value) {
    if (!value) return;
    for (const char* p = value; *p; ++p) {
        if (*p == '"' || *p == '\\') payload += '\\';
        payload += *p;
    }
}

void appendStringField(String& payload, const char* key, const char* value) {
    payload += "\"";
    payload += key;
    payload += "\":\"";
    appendEscaped(payload, value ? value : "");
    payload += "\"";
}

void appendCapabilityArray(String& payload, Capability* head, bool active) {
    payload += "[";
    bool first = true;
    for (Capability* c = head; c; c = c->next) {
        if (c->active != active) continue;
        if (!first) payload += ",";
        payload += "\"";
        appendEscaped(payload, c->id);
        payload += "\"";
        first = false;
    }
    payload += "]";
}

}  // namespace

const Board& current() { return CURRENT_BOARD; }

bool capabilityAllowed(const char* capabilityId) {
#if defined(FORGEKEY_EPAPER)
    if (streq(capabilityId, "epaper_pm")) return true;
    if (streq(capabilityId, "status_led")) return FORGEKEY_STATUS_LED_PIN >= 0;
    if (streq(capabilityId, "button")) return FORGEKEY_BUTTON_PIN >= 0;
    if (streq(capabilityId, "mmwave_presence")) return mmwaveConfigured();
    return !capabilityHasAnyPin(capabilityId);
#else
    if (streq(capabilityId, "status_led")) return FORGEKEY_STATUS_LED_PIN >= 0;
    if (streq(capabilityId, "button")) return FORGEKEY_BUTTON_PIN >= 0;
    if (streq(capabilityId, "mmwave_presence")) return mmwaveConfigured();
    return true;
#endif
}

int statusLedPin() { return FORGEKEY_STATUS_LED_PIN; }
bool statusLedActiveLow() { return FORGEKEY_STATUS_LED_ACTIVE_LOW != 0; }
int buttonPin() { return FORGEKEY_BUTTON_PIN; }
bool mmwaveConfigured() { return FORGEKEY_MMWAVE_RX_PIN >= 0 && FORGEKEY_MMWAVE_TX_PIN >= 0; }
int mmwaveRxPin() { return FORGEKEY_MMWAVE_RX_PIN; }
int mmwaveTxPin() { return FORGEKEY_MMWAVE_TX_PIN; }
uint32_t mmwaveBaud() { return (uint32_t)FORGEKEY_MMWAVE_BAUD; }

bool checkActiveCapabilityPins(Capability* head) {
    bool ok = true;
    const Board& b = current();
    Serial.printf("[BOARD] manifest=%s (%s), pins=%u, buses=%u\n",
                  b.id, b.displayName, (unsigned)b.pinCount, (unsigned)b.busCount);

    for (Capability* c = head; c; c = c->next) {
        if (!c->active) continue;
        if (!capabilityAllowed(c->id)) {
            Serial.printf("[BOARD] skip %s: not allowed by manifest %s\n", c->id, b.id);
            c->active = false;
            ok = false;
        }
    }

    for (size_t i = 0; i < b.pinCount; ++i) {
        const PinClaim& a = b.pins[i];
        if (a.pin < 0) continue;
        bool ownerActive = false;
        for (Capability* c = head; c; c = c->next) {
            if (c->active && streq(c->id, a.owner)) {
                ownerActive = true;
                break;
            }
        }
        if (!ownerActive) continue;
        if (a.unsafe && !ownerHasPin(a.owner, a.pin)) {
            Serial.printf("[BOARD] skip %s: GPIO%d is unsafe and not explicitly owned\n", a.owner, a.pin);
            ok = false;
        }
        for (size_t j = i + 1; j < b.pinCount; ++j) {
            const PinClaim& other = b.pins[j];
            if (other.pin != a.pin || streq(other.owner, a.owner)) continue;
            bool otherActive = false;
            for (Capability* c = head; c; c = c->next) {
                if (c->active && streq(c->id, other.owner)) {
                    otherActive = true;
                    break;
                }
            }
            if (otherActive) {
                Serial.printf("[BOARD] conflict GPIO%d: %s/%s vs %s/%s; disabling %s\n",
                              a.pin, a.owner, a.signal, other.owner, other.signal, other.owner);
                for (Capability* c = head; c; c = c->next) {
                    if (streq(c->id, other.owner)) c->active = false;
                }
                ok = false;
            }
        }
        if (pinInList(a.pin, b.bootStrapPins, b.bootStrapPinCount)) {
            Serial.printf("[BOARD] GPIO%d is a boot strap pin; owner=%s signal=%s\n", a.pin, a.owner, a.signal);
        }
    }
    return ok;
}

void appendHealthJson(String& payload, Capability* head) {
    const Board& b = current();
    payload += ",\"hardware\":{";
    appendStringField(payload, "board_id", b.id);
    payload += ",";
    appendStringField(payload, "board_name", b.displayName);
    payload += ",\"pins\":[";
    bool first = true;
    for (size_t i = 0; i < b.pinCount; ++i) {
        const PinClaim& p = b.pins[i];
        if (p.pin < 0) continue;
        if (!first) payload += ",";
        payload += "{\"gpio\":" + String(p.pin) + ",";
        appendStringField(payload, "owner", p.owner);
        payload += ",";
        appendStringField(payload, "signal", p.signal);
        payload += ",\"unsafe\":";
        payload += p.unsafe ? "true" : "false";
        payload += "}";
        first = false;
    }
    payload += "],\"active_capabilities\":";
    appendCapabilityArray(payload, head, true);
    payload += ",\"skipped_capabilities\":";
    appendCapabilityArray(payload, head, false);
    payload += "}";
}

}  // namespace BoardManifest
