# ForgeKey Device Lifecycle

How a freshly flashed XIAO ESP32-S3 Sense gets onto the OpenMakerSpace fleet,
keeps itself updated, and ships periodic photos for the OMS area-imagery feed.

This document covers fo-0z9 (provisioning, periodic photo upload, OTA). For
the people-counting pipeline see [PEOPLE_COUNTER.md](PEOPLE_COUNTER.md).

## High-level flow

```
flash → boot → wifi → camera → registered? ─no→ POST /register/ → store creds
                                       │
                                       yes
                                       ▼
                       ┌── MQTT subscribe forgekey/<mac>/firmware
                       │           │
                       │       on payload {url, sha256, version, mandatory}
                       │           ▼
                       │    download + verify SHA-256 + flash + reboot
                       │           │
                       │     mark_app_valid on first successful occupancy publish
                       │
                       ├── every 2s:  detect → publish occupancy on change
                       │
                       └── every 300s: capture JPEG → POST /devices/<mac>/photo/
                                       (skipped if no motion in last 30s)
```

## Compile-time configuration

All endpoint and credential knobs live in `src/provisioning/device_config.h`
and can be overridden via `-D` flags in `platformio.ini`:

| Macro | Default | Purpose |
|-------|---------|---------|
| `OMS_HOST` | `oms.openmakerspace.org` | OMS HTTPS host for `/api/forgekey/...` |
| `OMS_PORT` | `443` | TLS port |
| `FORGEKEY_PROVISIONING_TOKEN` | `REPLACE_ME_PROVISIONING_TOKEN` | Shared bearer for first-boot register POST |
| `FORGEKEY_SENSOR_KIND` | `people-counter` | Capability tag sent at register time |
| `FORGEKEY_FIRMWARE_VERSION` | `0.1.0` | Reported in registration payload + OTA logs |
| `PHOTO_UPLOAD_INTERVAL_MS` | `300000` | Periodic photo cadence |
| `PHOTO_UPLOAD_MOTION_WINDOW_MS` | `30000` | Skip photo if no motion seen this recently |

WiFi SSID/PSK and MQTT broker are still set at the top of `src/main.cpp` for
v1; captive-portal-based credential entry is tracked as a follow-up.

## Provisioning (first boot)

On every boot the device:

1. Connects WiFi using the baked-in credentials.
2. Bumps a persistent `boot_count` in NVS namespace `forgekey`.
3. If NVS does not yet contain a `dev_id` + `jwt`, runs `runProvisioning()`:
   - Captures one JPEG (`CameraManager::captureJpeg`, frame2jpg-encoded).
   - POSTs `multipart/form-data` to
     `https://OMS_HOST/api/forgekey/devices/register/` with two parts:
     * `metadata` — JSON `{mac_address, firmware_version, sensor_kind,
       boot_count, free_heap, ip}`
     * `photo` — `image/jpeg` of the area
   - Header `X-ForgeKey-Provisioning-Token: <FORGEKEY_PROVISIONING_TOKEN>`.
   - Expects 2xx with JSON body
     `{device_id, mqtt_topic_for_firmware, mqtt_topic_for_pings, jwt_token}`.
   - Persists those four fields into NVS.
4. Subsequent boots skip the registration POST.

If registration fails (network, OMS down, bad token), the device keeps running
locally — counting people and printing to serial — and retries the POST on the
next boot. There is no infinite retry loop in `setup()` to keep boot fast.

### Re-registration trigger

If a `/photo/` POST returns `401 Unauthorized` the device clears its NVS
credentials. The next reboot re-runs the registration flow, picking up a new
`jwt_token`. (The next photo cycle simply skips itself with `AuthExpired`;
re-registration mid-loop is out of scope for v1.)

## Periodic photo upload

The main loop checks `PhotoUploader::shouldUpload()` each tick:

- True if the last upload was ≥ `PHOTO_UPLOAD_INTERVAL_MS` ago **and** motion
  has been observed within the last `PHOTO_UPLOAD_MOTION_WINDOW_MS`.
- The very first post-boot upload is unconditional so OMS sees the device
  immediately after registration.
- Any frame that the detector flags as `motionDetected` or that contains a
  detected person counts as motion.

When eligible:

- Capture a fresh JPEG.
- POST `multipart/form-data` (single `photo` part) to
  `https://OMS_HOST/api/forgekey/devices/<mac>/photo/` with
  `Authorization: Bearer <jwt_token>`.
- Status codes:
  * `2xx` → record `lastUploadMs`, free buffer, done.
  * `401` → clear NVS credentials (forces re-register on next reboot).
  * `5xx` / other → log + continue; next interval will retry.

Photos are JPEG-encoded from the same QVGA grayscale frame the detector uses,
which keeps memory pressure low (one camera config, one PSRAM frame buffer).

## OTA firmware updates

OTA is dispatched over MQTT, not pulled. After registration the device
subscribes to the topic returned by the server in `mqtt_topic_for_firmware`
(typically `forgekey/<mac>/firmware`).

### Dispatch payload

```json
{
  "url": "https://oms.openmakerspace.org/static/firmware/0.2.0.bin",
  "sha256": "f3b5...64hex",
  "version": "0.2.0",
  "mandatory": false
}
```

### Apply flow

`OtaUpdater::apply()`:

1. Splits the URL (`http`/`https` + host + port + path).
2. Opens a `WiFiClient` (or `WiFiClientSecure` with `setInsecure()` for v1).
3. Issues a manual `GET` and parses status + headers (requires
   `Content-Length`; chunked transfer encoding is not yet supported).
4. Calls `Update.begin(contentLength)` to open the inactive OTA partition.
5. Streams the body 1 KB at a time:
   * Writes each chunk to `Update.write()`.
   * Feeds each chunk into `mbedtls_sha256_update()` in parallel.
6. Compares the computed digest to the dispatch payload's `sha256`.
   Mismatch → `Update.abort()`, no partition switch, log + return.
7. `Update.end(true)` finalizes; `ESP.restart()` reboots into the new image.

Non-mandatory updates are deferred if a photo upload happened in the last 5
seconds; mandatory updates apply immediately.

### Rollback safety

The new partition is written but **not** marked stable. After reboot the
ESP-IDF bootloader runs the new firmware in `ESP_OTA_IMG_PENDING_VERIFY`
state. The very first successful occupancy publish in `loop()` triggers
`OtaUpdater::markStableIfPending()`, which calls
`esp_ota_mark_app_valid_cancel_rollback()`. If the new firmware crashes, soft-
bricks WiFi, or fails to talk to MQTT before that point, a power cycle rolls
back to the previous partition automatically.

## Storage layout

NVS namespace `forgekey`:

| Key | Type | Description |
|-----|------|-------------|
| `boots` | uint32 | Persistent boot counter |
| `dev_id` | str | OMS-assigned device id |
| `fw_topic` | str | OTA dispatch MQTT topic |
| `p_topic` | str | Occupancy publish MQTT topic |
| `jwt` | str | Bearer for HTTPS uploads + MQTT auth |

A factory reset (re-registration) is `provisioning.clear()` from the field —
or an over-the-air firmware that wipes the namespace.

## Security model (v1)

- **Transport**: TLS to OMS, currently with `setInsecure()` (no CA pinning).
  Pinning is tracked as a follow-up; rotation can ship over OTA.
- **Provisioning**: shared bearer baked into the firmware. Devices are
  physically controlled in v1; rotation arrives over OTA.
- **Per-device auth**: each device gets a unique `jwt_token` from OMS at
  registration, used for HTTPS uploads and MQTT auth thereafter.
- **OTA integrity**: SHA-256 verification of the downloaded image plus the
  ESP-IDF two-slot rollback scheme. Signed firmware is v2.

## Manual smoke test (hardware)

After `~/.platformio/penv/bin/platformio run --target upload`:

1. Watch the serial monitor — confirm WiFi connect and `provision: first
   boot — registering with OMS`.
2. Confirm the registration POST appears in OMS access logs and the device
   shows up in the OMS admin device list.
3. Within 5 minutes of motion, confirm a photo lands in the OMS device
   gallery for that MAC.
4. From OMS admin, dispatch a fake firmware (any signed `firmware.bin`) on
   `forgekey/<mac>/firmware`. Confirm the serial log shows
   `ota: downloading → installed → rebooting`.
5. After reboot, confirm the new firmware version appears in the next
   registration ping or occupancy log line, and that
   `ota: marked running partition as valid` appears once a publish lands.

## Out of scope (filed as follow-ups)

- Captive-portal WiFi credential entry
- Offline queue + exponential backoff for unreachable OMS
- Signed firmware images (currently SHA-256 + TLS only)
- Multi-channel update streams (stable / beta / dev)
- TLS CA pinning (currently `setInsecure()`)
