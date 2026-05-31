#ifndef FORGEKEY_CONFIG_BLE_DESIRED_STATE_H
#define FORGEKEY_CONFIG_BLE_DESIRED_STATE_H

#include <Arduino.h>
#include <ArduinoJson.h>

namespace ble_desired_state {

constexpr size_t kBleListMax = 16;
constexpr size_t kBleMacLen = 12;
constexpr size_t kBleNamespaceMax = 32;

struct BleConfig {
    bool scannerEnabled;
    bool beaconEnabled;
    bool relayEnabled;
    bool equipmentEnabled;
    unsigned long scanIntervalMs;
    uint16_t scanDurationS;
    int8_t rssiThreshold;
    bool rawMacEnabled;
    char identityMode[12];  // "hashed" or "ephemeral"
    char siteNamespace[kBleNamespaceMax];
    char allowlist[kBleListMax][kBleMacLen + 1];
    uint8_t allowlistCount;
    char denylist[kBleListMax][kBleMacLen + 1];
    uint8_t denylistCount;
};

const BleConfig& current();
void begin();
bool apply(JsonVariantConst desired, String& detail);
void appendConfigJson(String& out);

bool macAllowed(const char* bareMac);
String publicDeviceId(const char* bareMac, unsigned long epoch);

}  // namespace ble_desired_state

#endif
