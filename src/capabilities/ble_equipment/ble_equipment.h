#ifndef FORGEKEY_CAPABILITIES_BLE_EQUIPMENT_H
#define FORGEKEY_CAPABILITIES_BLE_EQUIPMENT_H

#include <Arduino.h>

namespace BleEquipment {

bool isActive();

// Runtime enable/disable. Returns previous state.
bool setEnabled(bool on);

// Clear all equipment tags (for forget_ble command).
void clearTags();

// Equipment tag entry (for configuration)
struct EquipmentTag {
    char name[32];
    char mac[13];           // bare MAC for mac-type tags
    char ibeacon_uuid[37];  // UUID for ibeacon-type tags
    char type[8];           // "mac" or "ibeacon"
    char zone[8];           // "near", "mid", "far"
    bool active;
};

// Set tags from raw JSON payload. Returns true on success.
bool setTagsFromJson(const uint8_t* payload, unsigned int length);

// Get configured tag count
int getTagCount();

// Get tag by index
const EquipmentTag* getTag(int index);

}  // namespace BleEquipment

#endif
