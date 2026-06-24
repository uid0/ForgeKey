#ifndef FORGEKEY_BOARD_MANIFEST_H
#define FORGEKEY_BOARD_MANIFEST_H

#include <Arduino.h>
#include "capabilities/capability.h"
#include "power/power_manager.h"

namespace BoardManifest {

enum class PullExpectation : uint8_t {
    None,
    PullUp,
    PullDown,
    External,
    Driven,
};

struct PinClaim {
    int pin;
    const char* owner;
    const char* signal;
    PullExpectation pull;
    bool output;
    bool unsafe;
};

struct BusManifest {
    const char* name;
    const char* owner;
    int sda;
    int scl;
    int sck;
    int miso;
    int mosi;
    int rx;
    int tx;
};

struct Board {
    const char* id;
    const char* displayName;
    const PinClaim* pins;
    size_t pinCount;
    const int* bootStrapPins;
    size_t bootStrapPinCount;
    const int* adcPins;
    size_t adcPinCount;
    const BusManifest* buses;
    size_t busCount;
    PowerManager::BatteryConfig battery;
};

const Board& current();
bool capabilityAllowed(const char* capabilityId);
int statusLedPin();
bool statusLedActiveLow();
int buttonPin();
bool mmwaveConfigured();
int mmwaveRxPin();
int mmwaveTxPin();
uint32_t mmwaveBaud();
bool badgeReaderConfigured();
const PowerManager::BatteryConfig& batteryConfig();

// Deactivates active capabilities whose manifest claims conflict, use unsafe
// pins without explicit ownership, or are not allowed for this target. Must run
// after detectAll() and before CapabilityRegistry::setupAll().
bool checkActiveCapabilityPins(Capability* head);

// Appends JSON fields for health/status payloads. Caller must already be inside
// an object and pass a payload that can receive a leading comma.
void appendHealthJson(String& payload, Capability* head);

}  // namespace BoardManifest

#endif
