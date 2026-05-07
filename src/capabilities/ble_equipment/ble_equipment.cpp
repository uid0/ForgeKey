// Equipment tracking: detect ESP32-based BLE beacon tags and report
// their proximity zones to OMS via MQTT.

#ifndef FORGEKEY_DISABLE_BLE_EQUIPMENT

#include "../capability.h"
#include "ble_equipment.h"

#include <Arduino.h>
#include <nvs_flash.h>
#include <ArduinoJson.h>

#include "../../mqtt/mqtt_client.h"
#include "../../provisioning/device_config.h"

// Detection hysteresis
#ifndef BLE_EQUIP_DETECTED_THRESHOLD
#define BLE_EQUIP_DETECTED_THRESHOLD 3
#endif

#ifndef BLE_EQUIP_LOST_THRESHOLD
#define BLE_EQUIP_LOST_THRESHOLD 6
#endif

namespace BleEquipment {

bool detectFn();
void setupFn();
void tickFn();

namespace {

bool g_active = false;
bool g_enabled = true;

// Configured equipment tags
constexpr int MAX_TAGS = 16;
EquipmentTag g_tags[MAX_TAGS];
int g_tagCount = 0;

// Clear all tags
void clearTags() {
    g_tagCount = 0;
    for (int i = 0; i < MAX_TAGS; i++) {
        g_tags[i].active = false;
        memset(&g_tags[i], 0, sizeof(EquipmentTag));
    }
}

// Forward declarations
static void saveTagsToNvs();

// Set tags from raw JSON payload. Returns true on success.
static bool parseTagsFromJson(const uint8_t* payload, unsigned int length) {
    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) return false;

    const char* cmd = doc["cmd"] | "";
    if (strcmp(cmd, "set_equipment") != 0) return false;

    clearTags();

    JsonArray tags = doc["tags"];
    for (JsonObject tag : tags) {
        if (g_tagCount >= MAX_TAGS) break;

        EquipmentTag* t = &g_tags[g_tagCount];
        strncpy(t->name, tag["name"] | "unnamed", 31);
        t->name[31] = '\0';
        strncpy(t->mac, tag["mac"] | "", 12);
        t->mac[12] = '\0';
        strncpy(t->ibeacon_uuid, tag["ibeacon_uuid"] | "", 36);
        t->ibeacon_uuid[36] = '\0';
        strncpy(t->type, tag["type"] | "mac", 7);
        t->type[7] = '\0';
        strncpy(t->zone, tag["proximity_zone"] | "far", 7);
        t->zone[7] = '\0';
        t->active = true;
        g_tagCount++;
    }

    // Persist to NVS
    saveTagsToNvs();

    Serial.printf("[CAP/ble_equipment] configured %d equipment tags\n", g_tagCount);
    return true;
}

// Load tags from NVS
void loadTagsFromNvs() {
    nvs_handle handle;
    esp_err_t err = nvs_open("forgekey_ble", NVS_READONLY, &handle);
    if (err != ESP_OK) return;

    size_t len = 0;
    err = nvs_get_str(handle, "tags", nullptr, &len);
    if (err != ESP_OK || len == 0) {
        nvs_close(handle);
        return;
    }

    char* buf = (char*)malloc(len);
    if (!buf) {
        nvs_close(handle);
        return;
    }

    err = nvs_get_str(handle, "tags", buf, &len);
    if (err == ESP_OK) {
        setTagsFromJson((const uint8_t*)buf, len);
    }
    free(buf);
    nvs_close(handle);
}

// Save tags to NVS
void saveTagsToNvs() {
    StaticJsonDocument<1024> doc;
    doc["cmd"] = "set_equipment";
    JsonArray tags = doc.createNestedArray("tags");
    for (int i = 0; i < g_tagCount; i++) {
        if (!g_tags[i].active) continue;
        JsonObject t = tags.createNestedObject();
        t["name"] = g_tags[i].name;
        if (g_tags[i].mac[0]) t["mac"] = g_tags[i].mac;
        if (g_tags[i].ibeacon_uuid[0]) t["ibeacon_uuid"] = g_tags[i].ibeacon_uuid;
        t["type"] = g_tags[i].type;
        t["proximity_zone"] = g_tags[i].zone;
    }

    String json;
    serializeJson(doc, json);

    nvs_handle handle;
    esp_err_t err = nvs_open("forgekey_ble", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_set_str(handle, "tags", json.c_str());
        nvs_commit(handle);
        nvs_close(handle);
    }
}

// Determine proximity zone from RSSI
const char* zoneFromRssi(int8_t rssi) {
    if (rssi > -60) return "near";
    if (rssi > -75) return "mid";
    return "far";
}

// Check if a BLE device matches an equipment tag
bool matchTag(const char* mac, const char* uuid, uint16_t major, uint16_t minor, int tagIdx) {
    EquipmentTag* t = &g_tags[tagIdx];
    if (!t->active) return false;

    if (strcmp(t->type, "mac") == 0) {
        return strcmp(mac, t->mac) == 0;
    }
    if (strcmp(t->type, "ibeacon") == 0) {
        return strcmp(uuid, t->ibeacon_uuid) == 0;
    }
    return false;
}

// Report equipment event to OMS
void reportEvent(int tagIdx, const char* event, int8_t rssi) {
    if (!mqttClient.isConnected()) return;

    EquipmentTag* t = &g_tags[tagIdx];
    StaticJsonDocument<256> doc;
    doc["cmd_ack"] = "equipment";
    doc["event"] = event;
    doc["name"] = t->name;
    doc["mac"] = t->mac;
    doc["rssi"] = rssi;
    doc["zone"] = zoneFromRssi(rssi);

    String json;
    serializeJson(doc, json);

    if (mqttClient.publishEquipmentEvent(json.c_str())) {
        Serial.printf("[CAP/ble_equipment] %s: %s (rssi=%d zone=%s)\n",
                      event, t->name, rssi, zoneFromRssi(rssi));
    }
}

}  // namespace

bool isActive() { return g_active; }
int getTagCount() { return g_tagCount; }
const EquipmentTag* getTag(int index) {
    if (index < 0 || index >= g_tagCount) return nullptr;
    return &g_tags[index];
}

bool setEnabled(bool on) {
    bool prev = g_enabled;
    g_enabled = on;
    return prev;
}

void clearTags() {
    g_tagCount = 0;
    for (int i = 0; i < MAX_TAGS; i++) {
        g_tags[i].active = false;
        memset(&g_tags[i], 0, sizeof(EquipmentTag));
    }
}

bool setTagsFromJson(const uint8_t* payload, unsigned int length) {
    return ::BleEquipment::parseTagsFromJson(payload, length);
}

bool detectFn() {
    return true;  // BLE is built into ESP32-S3
}

void setupFn() {
    g_active = true;
    loadTagsFromNvs();
    Serial.printf("[CAP/ble_equipment] loaded %d tags from NVS\n", g_tagCount);
}

void tickFn() {
    // This capability doesn't scan on its own — it processes scan results
    // from the ble_scanner capability. The actual detection logic runs
    // when ble_scanner publishes its results. Config updates are handled
    // in main.cpp onConfigMessage.
}

}  // namespace BleEquipment

REGISTER_CAPABILITY(ble_equipment, "ble_equipment",
                    BleEquipment::detectFn,
                    BleEquipment::setupFn,
                    BleEquipment::tickFn,
                    "ble/equipment")

#endif  // FORGEKEY_DISABLE_BLE_EQUIPMENT
