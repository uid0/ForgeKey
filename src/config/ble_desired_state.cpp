#include "ble_desired_state.h"

#include <nvs_flash.h>
#include <esp_system.h>

#include "../provisioning/device_config.h"

#ifndef BLE_SCAN_INTERVAL_MS
#define BLE_SCAN_INTERVAL_MS 60000UL
#endif

#ifndef BLE_SCAN_DURATION_S
#define BLE_SCAN_DURATION_S 5
#endif

#ifndef BLE_SCAN_RSSI_THRESHOLD
#define BLE_SCAN_RSSI_THRESHOLD -90
#endif

namespace ble_desired_state {
namespace {
constexpr const char* kNvsNamespace = "fk_ble_cfg";
constexpr const char* kNvsKey = "desired";

BleConfig g_config = {
    FORGEKEY_BLE_ENABLED_DEFAULT != 0,
    FORGEKEY_BLE_ENABLED_DEFAULT != 0,
    FORGEKEY_BLE_ENABLED_DEFAULT != 0,
    FORGEKEY_BLE_ENABLED_DEFAULT != 0,
    BLE_SCAN_INTERVAL_MS,
    BLE_SCAN_DURATION_S,
    BLE_SCAN_RSSI_THRESHOLD,
    false,
    "hashed",
    "forgekey",
    {},
    0,
    {},
    0,
};

uint32_t g_bootSalt = 0;
bool g_loading = false;

void normalizeMac(const char* input, char* out) {
    size_t n = 0;
    if (!input) {
        out[0] = '\0';
        return;
    }
    for (const char* p = input; *p && n < kBleMacLen; ++p) {
        char c = *p;
        if (c == ':' || c == '-' || c == ' ') continue;
        if (c >= 'A' && c <= 'F') c = c - 'A' + 'a';
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
            out[n++] = c;
        }
    }
    out[n] = '\0';
}

bool listContains(char list[][kBleMacLen + 1], uint8_t count, const char* mac) {
    for (uint8_t i = 0; i < count; ++i) {
        if (strcmp(list[i], mac) == 0) return true;
    }
    return false;
}

uint32_t fnv1a(const char* text, uint32_t seed) {
    uint32_t hash = 2166136261UL ^ seed;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); p && *p; ++p) {
        hash ^= *p;
        hash *= 16777619UL;
    }
    return hash;
}

void parseList(JsonVariantConst value, char dest[][kBleMacLen + 1], uint8_t& count) {
    count = 0;
    if (!value.is<JsonArrayConst>()) return;
    for (JsonVariantConst item : value.as<JsonArrayConst>()) {
        if (count >= kBleListMax) break;
        char mac[kBleMacLen + 1];
        normalizeMac(item.as<const char*>(), mac);
        if (strlen(mac) != kBleMacLen) continue;
        snprintf(dest[count], kBleMacLen + 1, "%s", mac);
        count++;
    }
}

void save() {
    if (g_loading) return;
    StaticJsonDocument<1024> doc;
    JsonObject ble = doc.createNestedObject("ble");
    ble["scanner"] = g_config.scannerEnabled;
    ble["beacon"] = g_config.beaconEnabled;
    ble["relay"] = g_config.relayEnabled;
    ble["equipment"] = g_config.equipmentEnabled;
    ble["scan_interval_ms"] = g_config.scanIntervalMs;
    ble["scan_duration_s"] = g_config.scanDurationS;
    ble["rssi_threshold"] = g_config.rssiThreshold;
    ble["raw_mac_enabled"] = g_config.rawMacEnabled;
    ble["identity_mode"] = g_config.identityMode;
    ble["site_namespace"] = g_config.siteNamespace;
    JsonArray allowlist = ble.createNestedArray("allowlist");
    for (uint8_t i = 0; i < g_config.allowlistCount; ++i) allowlist.add(g_config.allowlist[i]);
    JsonArray denylist = ble.createNestedArray("denylist");
    for (uint8_t i = 0; i < g_config.denylistCount; ++i) denylist.add(g_config.denylist[i]);

    String json;
    serializeJson(doc, json);
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_str(handle, kNvsKey, json.c_str());
    nvs_commit(handle);
    nvs_close(handle);
}

}  // namespace

const BleConfig& current() { return g_config; }

void begin() {
    if (g_bootSalt == 0) g_bootSalt = esp_random();
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    size_t len = 0;
    if (nvs_get_str(handle, kNvsKey, nullptr, &len) != ESP_OK || len == 0) {
        nvs_close(handle);
        return;
    }
    char* buf = static_cast<char*>(malloc(len));
    if (!buf) {
        nvs_close(handle);
        return;
    }
    bool loaded = nvs_get_str(handle, kNvsKey, buf, &len) == ESP_OK;
    nvs_close(handle);
    if (loaded) {
        StaticJsonDocument<1024> doc;
        if (!deserializeJson(doc, buf, len)) {
            String ignored;
            g_loading = true;
            apply(doc.as<JsonVariantConst>(), ignored);
            g_loading = false;
        }
    }
    free(buf);
}

bool apply(JsonVariantConst desired, String& detail) {
    JsonVariantConst ble = desired["ble"];
    if (ble.isNull()) ble = desired;

    bool hasGlobal = !ble["enabled"].isNull();
    bool global = hasGlobal ? ble["enabled"].as<bool>() : false;
    g_config.scannerEnabled = !ble["scanner"].isNull()
        ? ble["scanner"].as<bool>()
        : (!ble["scanner_enabled"].isNull() ? ble["scanner_enabled"].as<bool>()
                                             : (hasGlobal ? global : g_config.scannerEnabled));
    g_config.beaconEnabled = !ble["beacon"].isNull()
        ? ble["beacon"].as<bool>()
        : (!ble["beacon_enabled"].isNull() ? ble["beacon_enabled"].as<bool>()
                                            : (hasGlobal ? global : g_config.beaconEnabled));
    g_config.relayEnabled = !ble["relay"].isNull()
        ? ble["relay"].as<bool>()
        : (!ble["relay_enabled"].isNull() ? ble["relay_enabled"].as<bool>()
                                           : (hasGlobal ? global : g_config.relayEnabled));
    g_config.equipmentEnabled = !ble["equipment"].isNull()
        ? ble["equipment"].as<bool>()
        : (!ble["equipment_enabled"].isNull() ? ble["equipment_enabled"].as<bool>()
                                               : (hasGlobal ? global : g_config.equipmentEnabled));

    g_config.scanIntervalMs = ble["scan_interval_ms"] | g_config.scanIntervalMs;
    g_config.scanDurationS = ble["scan_duration_s"] | g_config.scanDurationS;
    g_config.rssiThreshold = ble["rssi_threshold"] | g_config.rssiThreshold;
    g_config.rawMacEnabled = !ble["raw_mac_enabled"].isNull()
        ? ble["raw_mac_enabled"].as<bool>()
        : (!ble["publish_raw_mac"].isNull() ? ble["publish_raw_mac"].as<bool>() : g_config.rawMacEnabled);

    const char* mode = !ble["identity_mode"].isNull()
        ? ble["identity_mode"].as<const char*>()
        : (!ble["mac_id_mode"].isNull() ? ble["mac_id_mode"].as<const char*>() : g_config.identityMode);
    if (!mode || !*mode) mode = g_config.identityMode;
    if (strcmp(mode, "raw") == 0) {
        g_config.rawMacEnabled = true;
        mode = "hashed";
    }
    if (strcmp(mode, "hashed") != 0 && strcmp(mode, "ephemeral") != 0) {
        detail = "invalid_identity_mode";
        return false;
    }
    snprintf(g_config.identityMode, sizeof(g_config.identityMode), "%s", mode);

    const char* ns = ble["site_namespace"] | g_config.siteNamespace;
    snprintf(g_config.siteNamespace, sizeof(g_config.siteNamespace), "%s", ns && *ns ? ns : "forgekey");

    parseList(ble["allowlist"], g_config.allowlist, g_config.allowlistCount);
    parseList(ble["denylist"], g_config.denylist, g_config.denylistCount);

    if (g_config.scanIntervalMs < 10000UL) g_config.scanIntervalMs = 10000UL;
    if (g_config.scanDurationS < 1) g_config.scanDurationS = 1;
    if (g_config.scanDurationS > 30) g_config.scanDurationS = 30;
    if (g_config.scanIntervalMs < (unsigned long)g_config.scanDurationS * 1000UL) {
        g_config.scanIntervalMs = (unsigned long)g_config.scanDurationS * 1000UL;
    }

    save();
    detail = "applied";
    return true;
}

void appendConfigJson(String& out) {
    StaticJsonDocument<384> doc;
    doc["scanner"] = g_config.scannerEnabled;
    doc["beacon"] = g_config.beaconEnabled;
    doc["relay"] = g_config.relayEnabled;
    doc["equipment"] = g_config.equipmentEnabled;
    doc["scan_interval_ms"] = g_config.scanIntervalMs;
    doc["scan_duration_s"] = g_config.scanDurationS;
    doc["rssi_threshold"] = g_config.rssiThreshold;
    doc["raw_mac_enabled"] = g_config.rawMacEnabled;
    doc["identity_mode"] = g_config.identityMode;
    doc["site_namespace"] = g_config.siteNamespace;
    serializeJson(doc, out);
}

bool macAllowed(const char* bareMac) {
    char mac[kBleMacLen + 1];
    normalizeMac(bareMac, mac);
    if (strlen(mac) != kBleMacLen) return false;
    if (listContains(g_config.denylist, g_config.denylistCount, mac)) return false;
    if (g_config.allowlistCount > 0 && !listContains(g_config.allowlist, g_config.allowlistCount, mac)) {
        return false;
    }
    return true;
}

String publicDeviceId(const char* bareMac, unsigned long epoch) {
    if (g_config.rawMacEnabled) return String(bareMac);
    String material = String(g_config.siteNamespace) + ":" + bareMac;
    uint32_t seed = 0;
    const char* prefix = "bleh";
    if (strcmp(g_config.identityMode, "ephemeral") == 0) {
        seed = g_bootSalt ^ (uint32_t)(epoch / 3600000UL);
        prefix = "blee";
    }
    uint32_t h1 = fnv1a(material.c_str(), seed);
    uint32_t h2 = fnv1a(material.c_str(), h1 ^ 0x9e3779b9UL);
    char id[24];
    snprintf(id, sizeof(id), "%s_%08lx%08lx", prefix, (unsigned long)h1, (unsigned long)h2);
    return String(id);
}

}  // namespace ble_desired_state
