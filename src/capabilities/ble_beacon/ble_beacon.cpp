// BLE beacon capability: broadcasts a ForgeKey iBeacon so other devices
// and phones can discover us. Duty-cycled to save power (~10% duty).

#ifndef FORGEKEY_DISABLE_BLE_BEACON

#include "../capability.h"
#include "ble_beacon.h"

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEAdvertising.h>
#include <ArduinoJson.h>

#include "../../mqtt/mqtt_client.h"
#include "../../provisioning/device_config.h"

// Beacon advertising: 100ms on, 2460ms off (~10% duty cycle)
#ifndef BLE_BEACON_INTERVAL_MS
#define BLE_BEACON_INTERVAL_MS 2560UL
#endif

#ifndef BLE_BEACON_ADV_ON_MS
#define BLE_BEACON_ADV_ON_MS 100UL
#endif

// ForgeKey beacon UUID
#define FORGEKEY_BEACON_UUID_STR "A1B2C3D4-E5F6-7890-ABCD-EF1234567890"

namespace BleBeacon {

bool detectFn();
void setupFn();
void tickFn();

namespace {

bool g_active = false;
bool g_continuous = false;
bool g_enabled = true;

BLEAdvertising* pAdvertising = nullptr;
bool g_advertising = false;
unsigned long g_advStart = 0;

// Device identity for Major/Minor
char g_major[3] = {0};
char g_minor[5] = {0};

// iBeacon manufacturer data payload (27 bytes: length + type + company_id + subtype + size + uuid(16) + major(2) + minor(2) + rssi)
uint8_t g_ibeaconData[27];

void getMajorMinor() {
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    mac.toLowerCase();
    g_major[0] = mac.charAt(0);
    g_major[1] = mac.charAt(1);
    g_major[2] = '\0';
    strncpy(g_minor, mac.substring(8, 12).c_str(), 4);
    g_minor[4] = '\0';
}

// Build iBeacon manufacturer data payload
void buildIbeaconData() {
    // Header: length(1) + type(0xFF) + company_id_LE(2) + subtype(1) + size(1)
    g_ibeaconData[0] = 0x15;  // length of remaining data
    g_ibeaconData[1] = 0xFF;  // manufacturer specific data type
    g_ibeaconData[2] = 0x00;  // Apple company ID (0x004C) LE
    g_ibeaconData[3] = 0x4C;
    g_ibeaconData[4] = 0x02;  // iBeacon subtype
    g_ibeaconData[5] = 0x15;  // iBeacon size

    // UUID (big-endian)
    const char* uuidStr = FORGEKEY_BEACON_UUID_STR;
    for (int i = 0; i < 16; i++) {
        char hex[3] = {uuidStr[i * 2], uuidStr[i * 2 + 1], '\0'};
        g_ibeaconData[6 + i] = (uint8_t)strtol(hex, nullptr, 16);
    }

    // Major (big-endian)
    g_ibeaconData[22] = (uint8_t)strtoul(g_major, nullptr, 16);
    g_ibeaconData[23] = 0;

    // Minor (big-endian)
    g_ibeaconData[24] = (uint8_t)strtoul(g_minor, nullptr, 16);
    g_ibeaconData[25] = 0;

    // RSSI at 1m
    g_ibeaconData[26] = 0xC4;
}

void startAdv() {
    if (g_advertising) return;
    g_advertising = true;
    g_advStart = millis();

    // Build manufacturer data as std::string
    std::string mfgData;
    mfgData.append((char*)g_ibeaconData, 24);

    BLEAdvertisementData advData;
    advData.setManufacturerData(mfgData);

    pAdvertising->setAdvertisementData(advData);
    pAdvertising->setScanResponseData(advData);
    pAdvertising->setAdvertisementType((esp_ble_adv_type_t)0x00);  // ADV_TYPE_IND
    pAdvertising->start();
}

void stopAdv() {
    if (!g_advertising) return;
    g_advertising = false;
    pAdvertising->stop();
}

}  // namespace

bool isActive() { return g_active; }
bool isContinuous() { return g_continuous; }

bool setEnabled(bool on) {
    bool prev = g_enabled;
    g_enabled = on;
    if (on) {
        startAdv();
    } else {
        stopAdv();
    }
    return prev;
}

bool setContinuous(bool on) {
    if (on == g_continuous) return false;
    g_continuous = on;
    if (on) {
        startAdv();
    } else {
        stopAdv();
    }
    return true;
}

bool detectFn() {
    return true;  // BLE is built into ESP32-S3
}

void setupFn() {
    g_active = true;
    getMajorMinor();
    buildIbeaconData();

    BLEDevice::init("ForgeKey");

    // Set advertising parameters
    pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->setAdvertisementType((esp_ble_adv_type_t)0x00);  // ADV_TYPE_IND

    Serial.printf("[CAP/ble_beacon] initialized (major=%s minor=%s duty_cycle=%.0f%%)\n",
                  g_major, g_minor,
                  (double)BLE_BEACON_ADV_ON_MS / BLE_BEACON_INTERVAL_MS * 100.0);
}

void tickFn() {
    unsigned long now = millis();

    // Duty cycle: advertise for BLE_BEACON_ADV_ON_MS every BLE_BEACON_INTERVAL_MS
    if (!g_advertising && g_enabled) {
        if (now - g_advStart >= BLE_BEACON_INTERVAL_MS || g_advStart == 0) {
            startAdv();
        }
    } else {
        if (now - g_advStart >= BLE_BEACON_ADV_ON_MS) {
            stopAdv();
        }
    }
}

}  // namespace BleBeacon

REGISTER_CAPABILITY(ble_beacon, "ble_beacon",
                    BleBeacon::detectFn,
                    BleBeacon::setupFn,
                    BleBeacon::tickFn,
                    "ble/beacons")

#endif  // FORGEKEY_DISABLE_BLE_BEACON
