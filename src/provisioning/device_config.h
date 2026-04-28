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

// Periodic photo cadence in milliseconds (300s per fo-0z9).
#ifndef PHOTO_UPLOAD_INTERVAL_MS
#define PHOTO_UPLOAD_INTERVAL_MS 300000UL
#endif

// Skip the periodic photo if no motion has been detected for this many
// milliseconds. Saves bandwidth when the room is dark/empty.
#ifndef PHOTO_UPLOAD_MOTION_WINDOW_MS
#define PHOTO_UPLOAD_MOTION_WINDOW_MS 30000UL
#endif

#endif
