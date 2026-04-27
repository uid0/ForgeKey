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
| `FORGEKEY_AP_PASSWORD` | `12345678` | Captive-portal AP password (printed on sticker) |

The MQTT broker host/port are still set at the top of `src/main.cpp`. WiFi
SSID/PSK are no longer baked in — see
[WiFi provisioning (captive portal)](#wifi-provisioning-captive-portal).

## WiFi provisioning (captive portal)

A freshly flashed device has no WiFi credentials, so it can't talk to OMS yet.
The first-boot flow uses a captive portal AP (built on
[`tzapu/WiFiManager`](https://github.com/tzapu/WiFiManager)) to collect
credentials from a phone or laptop on-site:

1. On boot, `WifiSetup::connectOrPortal()` tries the SSID/PSK saved by
   WiFiManager in NVS. If the connect succeeds, normal flow resumes.
2. If no creds are saved (or the saved network is unreachable), the device
   raises an open WiFi AP:
   - **SSID**: `ForgeKey-Setup-XXXX` where `XXXX` is the last four hex digits
     of the device MAC, uppercased (e.g. `ForgeKey-Setup-A4F9`).
   - **Password**: `12345678` (override at build time with
     `-DFORGEKEY_AP_PASSWORD=...`).
   - A DNS server on `192.168.4.1` answers any hostname, which triggers the
     iOS / Android "Sign in to network" captive-portal flow automatically.
3. The user picks their SSID from the scanned list, types the PSK, hits
   *Save*. WiFiManager persists the creds and reboots.
4. After reboot the device re-runs step 1, this time succeeding, then
   continues to OMS registration / occupancy publishing.

`connectOrPortal()` blocks. If the user never completes the form, the device
sits in AP mode forever — by design, since there's nothing useful it can do
without WiFi. A timeout can be passed in if needed.

### Sticker label format

Every shipped device should carry a sticker showing its AP credentials:

```
ForgeKey-Setup-XXXX
Pwd: 12345678
```

Replace `XXXX` with the last four hex chars of the printed MAC address.

### Forgetting WiFi (re-provisioning a deployed device)

To move a device to a new network without physically reflashing it, publish
the following JSON to its config topic:

```
forgekey/<mac>/config
{"cmd": "forget_wifi"}
```

The device clears its saved WiFi creds and reboots back into the captive
portal. Use this when handing a device to a new makerspace.

> Note: the device must currently be online to receive this command. A device
> that has lost WiFi entirely needs a power-cycle reset by hand (a future
> revision will add a physical button + GPIO trigger).

## Provisioning (first boot)

On every boot the device:

1. Connects WiFi via the captive-portal flow above. If WiFi creds aren't
   saved yet this blocks until a user completes the portal form.
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
  "signature": "MEUCIQ...base64-DER-ECDSA(P-256)",
  "version": "0.2.0",
  "mandatory": false
}
```

`signature` is the base64-encoded ECDSA(P-256)-over-SHA-256 signature of the
raw firmware bytes, produced by OMS using the private key matching the public
key baked into firmware (`src/security/firmware_pubkey.h`). Devices reject
any dispatch missing the signature field; see "Signed firmware" below.

### Apply flow

`OtaUpdater::apply()`:

1. Splits the URL (`http`/`https` + host + port + path).
2. Opens a `WiFiClient` (or `WiFiClientSecure` with `setCACert(kOmsCaPem)` for
   pinned TLS).
3. Issues a manual `GET` and parses status + headers (requires
   `Content-Length`; chunked transfer encoding is not yet supported).
4. Calls `Update.begin(contentLength)` to open the inactive OTA partition.
5. Streams the body 1 KB at a time:
   * Writes each chunk to `Update.write()`.
   * Feeds each chunk into `mbedtls_sha256_update()` in parallel.
6. Compares the computed digest to the dispatch payload's `sha256`.
   Mismatch → `Update.abort()`, no partition switch, log + return.
7. Base64-decodes the dispatch `signature` and runs
   `mbedtls_pk_verify(MBEDTLS_MD_SHA256, digest, signature)` against the
   baked-in ECDSA(P-256) public key. Mismatch → `Update.abort()`, log + return.
8. `Update.end(true)` finalizes; `ESP.restart()` reboots into the new image.

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
| `prov_tok` | str | OTA-rotated provisioning token (overrides compile-time default) |

A factory reset (re-registration) is `provisioning.clear()` from the field —
or an over-the-air firmware that wipes the namespace. `clear()` deliberately
preserves `boots` and `prov_tok` so a rotated token survives re-registration.

## Credential rotation

Devices subscribe to `forgekey/<mac>/config`. A payload of:

```json
{ "provisioning_token": "<new>", "valid_after": "2026-05-01T00:00:00Z" }
```

writes the new token to NVS (`prov_tok`). The device uses the rotated token
on its next re-registration; OMS coordinates the cutover by accepting both
the old and new tokens during the transition window, then revoking the old
one once all devices have ack'd. `valid_after` is parsed and logged but the
device does not enforce it — the back-end controls when the old token
stops being honoured by `/api/forgekey/devices/register/`.

## Security model

- **Transport**: TLS to OMS with the OMS root certificate baked into firmware
  (`src/security/oms_ca.h`). `WiFiClientSecure::setCACert()` is used in all
  three HTTPS paths (registration, photo upload, OTA download). MITM via a
  rogue public CA is rejected at the TLS handshake.
- **Provisioning bearer**: shared token baked into the firmware at build time
  via `FORGEKEY_PROVISIONING_TOKEN`. The token can be rotated post-deploy
  through the MQTT `forgekey/<mac>/config` channel; the rotated value lives
  in NVS and overrides the compile-time default.
- **Per-device auth**: each device gets a unique `jwt_token` from OMS at
  registration, used for HTTPS uploads and MQTT auth thereafter.
- **OTA integrity + authenticity**:
  * SHA-256 of the downloaded image must match the dispatch payload.
  * ECDSA(P-256) signature over the same digest must verify against the
    public key baked into firmware (`src/security/firmware_pubkey.h`).
  * Both checks happen *before* `Update.end()` — a failed check leaves the
    inactive partition aborted and never swaps the boot pointer.
  * The ESP-IDF two-slot rollback scheme catches runtime crashes after boot.

## Rotation procedures

### OMS TLS root certificate

OMS rotates its TLS chain (e.g. Let's Encrypt root migration):

1. Run `scripts/build/fetch-oms-ca.sh` against the new endpoint to refresh
   `src/security/oms_ca.h`.
2. During an overlap window, concatenate both old and new PEMs in the header
   so devices flashed with either build keep working.
3. Bump `FORGEKEY_FIRMWARE_VERSION`, build, push to the OTA channel, and
   wait for fleet uptake.
4. Once metrics show all devices on the new build, drop the old PEM and
   ship a final clean-up build.

### Firmware-signing keypair

To rotate the OTA signing keypair (suspected compromise, scheduled hygiene):

1. `scripts/build/gen-firmware-signing-key.sh` writes a fresh keypair under
   `.firmware-keys/` (gitignored).
2. Deploy the new private key to OMS as `FORGEKEY_FIRMWARE_SIGNING_KEY`,
   keeping the old key alongside for the transition.
3. `scripts/build/gen-firmware-signing-key.sh --update-header` rewrites
   `src/security/firmware_pubkey.h`. Concatenating two PEM blocks and
   teaching `firmware_verify::verifySignature` to try both is one option for
   true zero-downtime rotation; the simpler path is dual-publishing
   firmware (one signed by old key, one by new) targeted by version range.
4. Bump `FORGEKEY_FIRMWARE_VERSION`, build, dispatch.
5. After fleet uptake, retire the old private key in OMS.

### Provisioning bearer token

For rotation post-deploy:

1. Generate the new shared token.
2. Publish the rotation message to each device's `forgekey/<mac>/config`
   topic. The device persists it to NVS immediately and acknowledges by
   logging the new token's prefix to serial.
3. Once OMS metrics show all devices ack'd (or the rollout window expires),
   stop accepting the old token at `/api/forgekey/devices/register/`.
4. Future builds should also bake the new token as the compile-time default
   so freshly flashed devices skip the rotation step.

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

- Offline queue + exponential backoff for unreachable OMS
- Multi-channel update streams (stable / beta / dev)
- ESP32 hardware Secure Boot v2 (fuse-burning, irreversible)
- mTLS (per-device client cert) — current model relies on JWT bearer for
  per-device auth; mTLS revisit if the attack surface widens
