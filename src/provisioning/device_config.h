#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

// Compile-time configuration overrides for the OMS endpoint and per-device
// bootstrap identity. Override via -D build flags in platformio.ini
// (preferred for CI / manufacturing) or by editing this header.

#ifndef OMS_HOST
#define OMS_HOST "dms.openmakersuite.net"
#endif

#ifndef OMS_PORT
#define OMS_PORT 443
#endif

// Short-lived or per-device bearer used in the X-ForgeKey-Provisioning-Token
// header for /api/forgekey/devices/enroll/. Manufacturing should mint a
// unique token per device (or per small batch with a short expiry) and bind it
// to the MAC, device class, claim code, and manufacturing record in OMS.
#ifndef FORGEKEY_BOOTSTRAP_TOKEN
#ifdef FORGEKEY_PROVISIONING_TOKEN
// Backward-compatible build flag alias while CI/manufacturing moves to the
// per-device bootstrap name. Do not use this for new shared fleet secrets.
#define FORGEKEY_BOOTSTRAP_TOKEN FORGEKEY_PROVISIONING_TOKEN
#else
#define FORGEKEY_BOOTSTRAP_TOKEN "REPLACE_ME_BOOTSTRAP_TOKEN"
#endif
#endif

#ifndef FORGEKEY_BOOTSTRAP_CLAIM_CODE
#define FORGEKEY_BOOTSTRAP_CLAIM_CODE ""
#endif

#ifndef FORGEKEY_MANUFACTURING_RECORD_ID
#define FORGEKEY_MANUFACTURING_RECORD_ID ""
#endif

#ifndef FORGEKEY_SUPPORT_URL
#define FORGEKEY_SUPPORT_URL "https://openmakersuite.net/forgekey/support"
#endif

// Static identity broadcast at enrollment. The OMS enroll endpoint
// uses this to slot the device into the right capability bucket.
#ifndef FORGEKEY_SENSOR_KIND
#define FORGEKEY_SENSOR_KIND "people-counter"
#endif

// Embedded firmware version string. Bump on every release; the OTA
// flow reports it back to OMS so dispatchers can target stragglers.
#ifndef FORGEKEY_FIRMWARE_VERSION
#define FORGEKEY_FIRMWARE_VERSION "0.1.0"
#endif

// Build identity used by fleet OTA policy filters. PlatformIO envs override
// this with their exact environment name.
#ifndef FORGEKEY_BUILD_TARGET
#define FORGEKEY_BUILD_TARGET "seeed_xiao_esp32s3"
#endif

#ifndef FORGEKEY_BUILD_FRAMEWORK
#define FORGEKEY_BUILD_FRAMEWORK "arduino"
#endif

// First-boot fallback MQTT broker. The authoritative broker host/port/tls
// come back from the OMS enrollment response and are persisted to NVS;
// these values are only used before a successful enrollment (or if the
// stored values are wiped). Override per-build via -D flags.
#ifndef MQTT_BROKER_FALLBACK_HOST
#define MQTT_BROKER_FALLBACK_HOST "dms.openmakersuite.net"
#endif
#ifndef MQTT_BROKER_FALLBACK_PORT
#define MQTT_BROKER_FALLBACK_PORT 8883
#endif
#ifndef MQTT_BROKER_FALLBACK_USE_TLS
#define MQTT_BROKER_FALLBACK_USE_TLS 1
#endif

// Periodic photo cadence in milliseconds (300s per fo-0z9).
#ifndef PHOTO_UPLOAD_INTERVAL_MS
#define PHOTO_UPLOAD_INTERVAL_MS 300000UL
#endif

// Skip the periodic photo if no motion has been detected for this many
// milliseconds. Saves bandwidth when the room is dark/empty.
#ifndef PHOTO_UPLOAD_MOTION_WINDOW_MS
#define PHOTO_UPLOAD_MOTION_WINDOW_MS 30000UL
#endif

// ===== Temperature-sensor variant =====
// Enabled by defining FORGEKEY_TEMPERATURE_SENSOR (see the
// seeed_xiao_esp32s3_temperature env in platformio.ini). When enabled the
// firmware skips camera/detection/photo upload and instead samples a DHT 21
// (AM2301) on FORGEKEY_DHT_PIN, publishing readings to
// forgekey/<mac>/temperature_sensor/reading.

// GPIO connected to the DHT21 data line. GPIO 2 (D1 on the XIAO ESP32-S3
// header) is free in the temperature build (the camera pins are unused).
#ifndef FORGEKEY_DHT_PIN
#define FORGEKEY_DHT_PIN 2
#endif

// Sampling cadence. DHT21 needs ≥2s between reads; sampling slower keeps
// self-heating low and the published series small.
#ifndef TEMPERATURE_SAMPLE_INTERVAL_MS
#define TEMPERATURE_SAMPLE_INTERVAL_MS 30000UL
#endif

// ===== BLE compile-time toggles =====
// Each BLE capability can be disabled individually via -D flags.
// All are enabled by default (undefined).

// Disable the BLE scanner capability (periodic BLE advertisement scanning)
// #define FORGEKEY_DISABLE_BLE_SCANNER

// Disable the BLE beacon broadcast capability (ForgeKey iBeacon advertising)
// #define FORGEKEY_DISABLE_BLE_BEACON

// Disable the inter-device BLE relay capability (message forwarding via BLE GATT)
// #define FORGEKEY_DISABLE_BLE_RELAY

// Disable the equipment tracking capability (ESP32 beacon tag detection)
// #define FORGEKEY_DISABLE_BLE_EQUIPMENT

// Default BLE state at boot (1=enabled, 0=disabled). Can be overridden
// at runtime via the forgekey/<mac>/config topic with {"cmd":"set_ble",...}.
#ifndef FORGEKEY_BLE_ENABLED_DEFAULT
#define FORGEKEY_BLE_ENABLED_DEFAULT 1
#endif

#endif
