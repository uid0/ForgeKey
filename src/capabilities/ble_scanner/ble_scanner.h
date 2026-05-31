#ifndef FORGEKEY_CAPABILITIES_BLE_SCANNER_H
#define FORGEKEY_CAPABILITIES_BLE_SCANNER_H

#include <Arduino.h>

namespace BleScanner {

bool isActive();

// Runtime enable/disable. Returns previous state.
bool setEnabled(bool on);
void appendHealthJson(String& out);

}  // namespace BleScanner

#endif
