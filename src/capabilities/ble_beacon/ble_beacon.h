#ifndef FORGEKEY_CAPABILITIES_BLE_BEACON_H
#define FORGEKEY_CAPABILITIES_BLE_BEACON_H

namespace BleBeacon {

bool isActive();

bool setContinuous(bool on);
bool isContinuous();

// Runtime enable/disable. Returns previous state.
bool setEnabled(bool on);

}  // namespace BleBeacon

#endif
