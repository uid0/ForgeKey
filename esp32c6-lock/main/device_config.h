/*
 * Device configuration for ESP32-C6 lock build.
 * Auto-generated from Arduino project sources — do not edit manually.
 * Run scripts/esp32c6-extract-secrets.py to update.
 */

#ifndef FORGEKEY_DEVICE_CONFIG_H
#define FORGEKEY_DEVICE_CONFIG_H

/* OMS endpoint */
#ifndef OMS_HOST
#define OMS_HOST "dms.openmakersuite.net"
#endif

#ifndef OMS_PORT
#define OMS_PORT 443
#endif

/* Provisioning token (extracted from src/provisioning/device_config.h) */
#ifndef FORGEKEY_PROVISIONING_TOKEN
#define FORGEKEY_PROVISIONING_TOKEN "REPLACE_ME_PROVISIONING_TOKEN"
#endif

/* Sensor kind */
#ifndef FORGEKEY_SENSOR_KIND
#define FORGEKEY_SENSOR_KIND "cabinet-lock"
#endif

/* Firmware version */
#ifndef FORGEKEY_FIRMWARE_VERSION
#define FORGEKEY_FIRMWARE_VERSION "0.1.0"
#endif

/* MQTT broker fallback */
#ifndef MQTT_BROKER_FALLBACK_HOST
#define MQTT_BROKER_FALLBACK_HOST "dms.openmakersuite.net"
#endif

#ifndef MQTT_BROKER_FALLBACK_PORT
#define MQTT_BROKER_FALLBACK_PORT 8883
#endif

#ifndef MQTT_BROKER_FALLBACK_USE_TLS
#define MQTT_BROKER_FALLBACK_USE_TLS 1
#endif

/* Captive portal AP password */
#ifndef FORGEKEY_AP_PASSWORD
#define FORGEKEY_AP_PASSWORD "12345678"
#endif

#endif /* FORGEKEY_DEVICE_CONFIG_H */
