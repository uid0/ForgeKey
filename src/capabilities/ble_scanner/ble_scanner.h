#ifndef FORGEKEY_CAPABILITIES_BLE_SCANNER_H
#define FORGEKEY_CAPABILITIES_BLE_SCANNER_H

namespace BleScanner {

bool isActive();

// Runtime enable/disable. Returns previous state.
bool setEnabled(bool on);

}  // namespace BleScanner

#endif
