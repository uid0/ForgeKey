#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

// Compile-time configuration overrides for the OMS endpoint and the
// shared provisioning bearer token. Override either via -D build flags
// in platformio.ini (preferred for CI / per-build secrets) or by
// editing this header.

#ifndef OMS_HOST
#define OMS_HOST "dms.openmakersuite.net"
#endif

#ifndef OMS_PORT
#define OMS_PORT 443
#endif

// Bearer used in the X-ForgeKey-Provisioning-Token header for
// /api/forgekey/devices/register/. Devices are physically controlled
// in v1, so a shared token is acceptable; rotation arrives over OTA.
#ifndef FORGEKEY_PROVISIONING_TOKEN
#define FORGEKEY_PROVISIONING_TOKEN "REPLACE_ME_PROVISIONING_TOKEN"
#endif

// Static identity broadcast at registration. The OMS register endpoint
// uses this to slot the device into the right capability bucket.
#ifndef FORGEKEY_SENSOR_KIND
#define FORGEKEY_SENSOR_KIND "people-counter"
#endif

// Embedded firmware version string. Bump on every release; the OTA
// flow reports it back to OMS so dispatchers can target stragglers.
#ifndef FORGEKEY_FIRMWARE_VERSION
#define FORGEKEY_FIRMWARE_VERSION "0.1.0"
#endif

// First-boot fallback MQTT broker. The authoritative broker host/port/tls
// come back from the OMS registration response and are persisted to NVS;
// these values are only used before a successful registration (or if the
// stored values are wiped). Override per-build via -D flags.
#ifndef MQTT_BROKER_FALLBACK_HOST
#define MQTT_BROKER_FALLBACK_HOST "dms.openmakersuite.net"
#endif
#ifndef MQTT_BROKER_FALLBACK_PORT
#define MQTT_BROKER_FALLBACK_PORT 1883
#endif
#ifndef MQTT_BROKER_FALLBACK_USE_TLS
#define MQTT_BROKER_FALLBACK_USE_TLS 0
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

#endif
