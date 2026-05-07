// BLE scanner capability: periodically scans for BLE advertisements,
// deduplicates across cycles, detects iBeacons, and publishes results
// to forgekey/<mac>/ble/devices.

#ifndef FORGEKEY_DISABLE_BLE_SCANNER

#include "../capability.h"
#include "ble_scanner.h"

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <ArduinoJson.h>

#include "../../mqtt/mqtt_client.h"
#include "../../provisioning/device_config.h"

#ifndef BLE_SCAN_INTERVAL_MS
#define BLE_SCAN_INTERVAL_MS 60000UL
#endif

#ifndef BLE_SCAN_DURATION_S
#define BLE_SCAN_DURATION_S 5
#endif

#ifndef BLE_SCAN_RSSI_THRESHOLD
#define BLE_SCAN_RSSI_THRESHOLD -90
#endif

#ifndef BLE_MAX_DEVICES
#define BLE_MAX_DEVICES 50
#endif

#ifndef BLE_DEDUP_WINDOW
#define BLE_DEDUP_WINDOW 60
#endif

// ForgeKey beacon UUID (used to identify peer devices)
#define FORGEKEY_BEACON_UUID "A1B2C3D4-E5F6-7890-ABCD-EF1234567890"

namespace BleScanner {

bool detectFn();
void setupFn();
void tickFn();

namespace {

bool g_active = false;
bool g_enabled = true;
unsigned long g_lastScan = 0;
unsigned long g_scanStart = 0;
bool g_scanning = false;

// Deduplication ring buffer: stores last N MACs with timestamps
struct MacEntry {
    char mac[13];  // 12 hex chars + null
    unsigned long ts;
};

MacEntry g_macRing[BLE_DEDUP_WINDOW];
int g_macRingHead = 0;
int g_macRingCount = 0;

// Current scan results
struct BleDevice {
    char mac[13];
    int8_t rssi;
    bool isIbeacon;
    char uuid[37];  // 32 hex + hyphens + null
    uint16_t major;
    uint16_t minor;
};

BleDevice g_devices[BLE_MAX_DEVICES];
int g_deviceCount = 0;

// Own MAC for filtering
char g_ownMac[13] = {0};

void getBareMac(char* out) {
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    mac.toLowerCase();
    strncpy(out, mac.c_str(), 12);
    out[12] = '\0';
}

bool macInRing(const char* mac) {
    for (int i = 0; i < g_macRingCount; i++) {
        if (strcmp(g_macRing[i].mac, mac) == 0) return true;
    }
    return false;
}

void addMacToRing(const char* mac) {
    int idx = g_macRingHead % BLE_DEDUP_WINDOW;
    strncpy(g_macRing[idx].mac, mac, 12);
    g_macRing[idx].mac[12] = '\0';
    g_macRing[idx].ts = millis();
    g_macRingHead++;
    if (g_macRingCount < BLE_DEDUP_WINDOW) {
        g_macRingCount++;
    }
}

bool isForgeKeyBeacon(const char* uuid) {
    return strcmp(uuid, FORGEKEY_BEACON_UUID) == 0;
}

// Parse iBeacon data from manufacturer specific data
// iBeacon AD structure: 0x02 (length) 0xFF (manufacturer specific) + 0x02 (type)
// + 0x15 (subtype) + 16-byte UUID + 2-byte major + 2-byte minor + signal
bool parseIbeacon(const uint8_t* data, int length, char* uuid, uint16_t* major, uint16_t* minor) {
    if (length < 26) return false;  // 2 (len) + 1 (type) + 1 (subtype) + 16 (uuid) + 2 + 2 + 2 (RSSI)

    // Check iBeacon subtype 0x15
    if (data[1] != 0x15) return false;

    // Extract UUID (big-endian, starts at data[2])
    for (int i = 0; i < 16; i++) {
        sprintf(uuid + i * 2, "%02x", data[2 + i]);
    }
    // Insert hyphens at correct positions
    uuid[8] = '-'; uuid[13] = '-'; uuid[18] = '-'; uuid[23] = '-';
    uuid[28] = '\0';

    // Major and minor are big-endian after UUID
    *major = (data[18] << 8) | data[19];
    *minor = (data[20] << 8) | data[21];

    return true;
}

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        if (g_scanning == false) return;

        char mac[13];
        String macStr = advertisedDevice.getAddress().toString().c_str();
        macStr.replace(":", "");
        macStr.toLowerCase();
        strncpy(mac, macStr.c_str(), 12);
        mac[12] = '\0';

        // Filter: own MAC, RSSI threshold
        if (g_ownMac[0] && strcmp(mac, g_ownMac) == 0) return;
        if (advertisedDevice.getRSSI() < BLE_SCAN_RSSI_THRESHOLD) return;
        if (g_deviceCount >= BLE_MAX_DEVICES) return;

        // Check dedup window
        if (macInRing(mac)) return;

        addMacToRing(mac);

        // Store device
        BleDevice* dev = &g_devices[g_deviceCount];
        strncpy(dev->mac, mac, 12);
        dev->mac[12] = '\0';
        dev->rssi = advertisedDevice.getRSSI();
        dev->isIbeacon = false;
        dev->uuid[0] = '\0';
        dev->major = 0;
        dev->minor = 0;

        // Check for iBeacon in manufacturer specific data
        const char* mfgData = advertisedDevice.getManufacturerData().c_str();
        int mfgLen = advertisedDevice.getManufacturerData().length();
        if (mfgLen >= 26 && mfgData[1] == 0xFF) {
            if (parseIbeacon((const uint8_t*)mfgData, mfgLen,
                             dev->uuid, &dev->major, &dev->minor)) {
                dev->isIbeacon = true;
            }
        }

        g_deviceCount++;
    }
};

}  // namespace

bool isActive() { return g_active; }

bool setEnabled(bool on) {
    bool prev = g_enabled;
    g_enabled = on;
    return prev;
}

bool detectFn() {
    return true;  // BLE is built into ESP32-S3
}

void setupFn() {
    g_active = true;
    getBareMac(g_ownMac);

    BLEDevice::init("");
    BLEScan* pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(37);

    Serial.printf("[CAP/ble_scanner] initialized (interval=%lus duration=%ds rssi_threshold=%d max_devices=%d)\n",
                  (unsigned long)(BLE_SCAN_INTERVAL_MS / 1000), BLE_SCAN_DURATION_S,
                  BLE_SCAN_RSSI_THRESHOLD, BLE_MAX_DEVICES);
}

void tickFn() {
    unsigned long now = millis();

    // Check if scan duration has elapsed
    if (g_scanning && (now - g_scanStart >= (BLE_SCAN_DURATION_S * 1000UL))) {
        g_scanning = false;
        BLEDevice::getScan()->stop();
        Serial.printf("[CAP/ble_scanner] scan complete: %d devices found\n", g_deviceCount);

        if (g_deviceCount > 0 && mqttClient.isConnected()) {
            // Build JSON payload
            StaticJsonDocument<2048> doc;
            JsonArray devices = doc.createNestedArray("devices");

            for (int i = 0; i < g_deviceCount; i++) {
                BleDevice* dev = &g_devices[i];
                JsonObject entry = devices.createNestedObject();
                entry["mac"] = dev->mac;
                entry["rssi"] = dev->rssi;

                if (dev->isIbeacon) {
                    entry["type"] = "ibeacon";
                    entry["uuid"] = dev->uuid;
                    entry["major"] = dev->major;
                    entry["minor"] = dev->minor;

                    if (isForgeKeyBeacon(dev->uuid)) {
                        entry["peer"] = true;
                    }
                } else {
                    entry["type"] = "general";
                }
            }

            doc["count"] = g_deviceCount;
            doc["timestamp"] = now;

            String json;
            serializeJson(doc, json);

            if (mqttClient.publishBleDevices(json.c_str())) {
                Serial.printf("[CAP/ble_scanner] published: %d devices (%u bytes)\n",
                              g_deviceCount, (unsigned)json.length());
            } else {
                Serial.println("[CAP/ble_scanner] failed to publish BLE devices");
            }
        }

        // Reset for next scan
        g_deviceCount = 0;
        g_lastScan = now;
        return;
    }

    // Trigger scan if interval has elapsed and not currently scanning
    if (!g_scanning && g_enabled && (now - g_lastScan >= BLE_SCAN_INTERVAL_MS)) {
        g_scanning = true;
        g_scanStart = now;
        g_deviceCount = 0;
        BLEDevice::getScan()->start(BLE_SCAN_DURATION_S, false);
        Serial.printf("[CAP/ble_scanner] scan started (duration=%ds)\n", BLE_SCAN_DURATION_S);
    }
}

}  // namespace BleScanner

REGISTER_CAPABILITY(ble_scanner, "ble_scanner",
                    BleScanner::detectFn,
                    BleScanner::setupFn,
                    BleScanner::tickFn,
                    "ble/devices")

#endif  // FORGEKEY_DISABLE_BLE_SCANNER
