#ifndef FORGEKEY_CAPABILITIES_BLE_BEACON_H
#define FORGEKEY_CAPABILITIES_BLE_BEACON_H

#include <Arduino.h>

namespace BleBeacon {

bool isActive();

bool setContinuous(bool on);
bool isContinuous();

// Runtime enable/disable. Returns previous state.
bool setEnabled(bool on);
void appendHealthJson(String& out);

}  // namespace BleBeacon

#endif
