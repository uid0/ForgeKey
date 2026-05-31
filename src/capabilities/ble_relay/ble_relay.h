#ifndef FORGEKEY_CAPABILITIES_BLE_RELAY_H
#define FORGEKEY_CAPABILITIES_BLE_RELAY_H

#include <Arduino.h>

namespace BleRelay {

bool isActive();

// Runtime enable/disable. Returns previous state.
bool setEnabled(bool on);
void appendHealthJson(String& out);

}  // namespace BleRelay

#endif
