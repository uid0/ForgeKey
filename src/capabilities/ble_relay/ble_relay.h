#ifndef FORGEKEY_CAPABILITIES_BLE_RELAY_H
#define FORGEKEY_CAPABILITIES_BLE_RELAY_H

namespace BleRelay {

bool isActive();

// Runtime enable/disable. Returns previous state.
bool setEnabled(bool on);

}  // namespace BleRelay

#endif
