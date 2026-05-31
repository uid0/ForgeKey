# ForgeKey Device Lifecycle

How a freshly flashed XIAO ESP32-S3 Sense gets onto the OpenMakerSpace fleet,
keeps itself updated, and ships periodic photos for the OMS area-imagery feed.

This document covers fo-0z9 (provisioning, periodic photo upload, OTA). For
the people-counting pipeline see [PEOPLE_COUNTER.md](PEOPLE_COUNTER.md).

## Hardware artifact index

Repeatable hardware documentation now lives under [`hardware/`](hardware/).
Each device class has a machine-readable `pin-manifest.json`, rendered
`wiring.svg`, BOM, harness notes, and simulation notes. Wokwi projects are
included where community simulator parts can model the relevant hardware.

| Device class | Hardware directory | Simulation coverage |
|---|---|---|
| People counter | [`hardware/people-counter/`](hardware/people-counter/) | SVG/pin review only; XIAO ESP32-S3 Sense camera is not modeled. |
| Temperature sensor | [`hardware/temperature-sensor/`](hardware/temperature-sensor/) | Wokwi DHT22 stand-in for DHT21/AM2301 on GPIO 2. |
| Cabinet lock | [`hardware/cabinet-lock/`](hardware/cabinet-lock/) | Wokwi GPIO logic model for solenoid indicator and supervised inputs. |
| ePaper display | [`hardware/epaper-display/`](hardware/epaper-display/) | SVG/pin review only; Seeed UC8179 7.5 in panel is not modeled. |
| Future modules | [`hardware/future-modules/`](hardware/future-modules/) | Template for new device-class artifacts. |

## High-level flow

```
flash → boot → wifi → camera → registered? ─no→ POST /enroll/ → store cert
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
| `OMS_HOST` | `dms.openmakersuite.net` | OMS HTTPS host for `/api/forgekey/...` |
| `OMS_PORT` | `443` | TLS port |
| `FORGEKEY_BOOTSTRAP_TOKEN` | `REPLACE_ME_BOOTSTRAP_TOKEN` | Short-lived/per-device bearer for first-boot enrollment; legacy `FORGEKEY_PROVISIONING_TOKEN` is only a build-flag alias |
| `FORGEKEY_BOOTSTRAP_CLAIM_CODE` | empty | Human claim code printed on the device label and bound to the bootstrap record |
| `FORGEKEY_MANUFACTURING_RECORD_ID` | empty | Factory work-order/record identifier sent during enrollment |
| `FORGEKEY_SUPPORT_URL` | `https://openmakersuite.net/forgekey/support` | URL printed on the label and sent to OMS |
| `FORGEKEY_SENSOR_KIND` | `people-counter` | Capability tag sent at enrollment time |
| `FORGEKEY_FIRMWARE_VERSION` | `0.1.0` | Reported in enrollment payload + OTA logs |
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
   continues to OMS enrollment / occupancy publishing.

`connectOrPortal()` blocks. If the user never completes the form, the device
sits in AP mode forever — by design, since there's nothing useful it can do
without WiFi. A timeout can be passed in if needed.

### Physical label and QR format

Every shipped device carries a durable label with both human-readable text and
a QR code. The QR payload is a support/claim URL that contains no long-lived
secret; operators still authenticate to OMS before ownership is transferred.

Human-readable label:

```
ForgeKey <DEVICE_CLASS>
MAC: <12-hex-mac>
Claim: <XXXX-XXXX-XXXX>
Setup WiFi: ForgeKey-Setup-<last4> / 12345678
Support: <FORGEKEY_SUPPORT_URL>
```

QR payload:

```
<FORGEKEY_SUPPORT_URL>?mac=<12-hex-mac>&class=<device-class>&claim=<claim-code>
```

Required fields:

| Field | Source | Notes |
|-------|--------|-------|
| MAC | eFuse base MAC read at manufacturing | Use lowercase 12-hex without separators in QR; the text label may group it for readability. |
| Device class | `FORGEKEY_SENSOR_KIND` / `FORGEKEY_BUILD_TARGET` | Examples: `people-counter`, `temperature-sensor`, `cabinet-lock`. |
| Claim code | Manufacturing system | 12+ random base32 characters grouped with dashes; stored hashed in OMS and printed once. |
| Support URL | `FORGEKEY_SUPPORT_URL` | Must resolve to an operator-friendly claim/support page. |

The captive-portal AP SSID remains `ForgeKey-Setup-XXXX`, where `XXXX` is the
last four hex chars of the printed MAC address.

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

## Bootstrap identity and enrollment (first boot)

### Manufacturing record

Manufacturing creates a one-row bootstrap record before the unit leaves the
bench. That record is the root of trust for first enrollment and contains:

| Field | Description | Handling |
|-------|-------------|----------|
| MAC | eFuse base MAC for the module | Immutable device lookup key; printed on the label and encoded in the QR URL. |
| Device class | `people-counter`, `temperature-sensor`, `cabinet-lock`, etc. | Must match the firmware's `FORGEKEY_SENSOR_KIND`; OMS rejects mismatches. |
| Claim code | Random human code such as `FK7P-Q9CD-2M6R` | Printed on the label; store only a salted hash in OMS; mark single-use after owner claim. |
| Bootstrap token | Per-device or very small-batch bearer | Sent only in `X-ForgeKey-Bootstrap-Token`; expiry should be short and scoped to the MAC/class/record. |
| Manufacturing record ID | Work order / serial / fixture run ID | Sent as `manufacturing_record_id` for audit and support. |
| QR/support URL | Claim/support page | QR contains MAC, class, and claim code but not the bootstrap token. |
| Public-key status | Pending until first boot | OMS stores the device public key/CSR fingerprint on first successful enrollment. |

The bootstrap token replaces the former shared fleet provisioning token. New
builds should set `FORGEKEY_BOOTSTRAP_TOKEN` per device (or inject it into NVS
on the fixture). The legacy `FORGEKEY_PROVISIONING_TOKEN` macro is retained only
as a temporary build-flag alias and must not be used as a shared production
secret.

### Enrollment request

On every boot the device:

1. Connects WiFi via the captive-portal flow above. If WiFi creds are not saved
   yet, this blocks until an operator completes the portal form.
2. Bumps a persistent `boot_count` in NVS namespace `forgekey`.
3. If NVS does not yet contain a `dev_id`, client certificate, and private key,
   runs the enrollment flow:
   - Generates a P-256 keypair on-device. The private key never leaves the
     device except being stored in NVS as `c_key`.
   - Builds a CSR with subject `CN=forgekey-<mac>`.
   - POSTs `multipart/form-data` to
     `https://OMS_HOST/api/forgekey/devices/enroll/` with:
     * `metadata` — JSON including `mac_address`, `firmware_version`,
       `sensor_kind`, `boot_count`, `free_heap`, `ip`, `csr_pem`,
       `device_public_key_pem`, `bootstrap_claim_code`,
       `manufacturing_record_id`, `support_url`, `unique_chip_id`,
       `flash_memory_id`, and `chip_info`.
     * `photo` — `image/jpeg` of the area for imaging builds only; lock and
       temperature builds send metadata only.
   - Sends headers `X-ForgeKey-Bootstrap-Token: <token>` and
     `X-ForgeKey-Claim-Code: <claim-code>`.
   - Expects 2xx with JSON body containing at least `device_id` and a signed
     `client_certificate_pem`/`certificate_pem`; OMS may also return MQTT
     broker policy, topics, command public key, and asset ID.
   - Persists the device id, certificate, private key, command public key, MQTT
     topics, and broker policy into NVS.
4. Subsequent boots skip enrollment and use the persisted certificate bundle.

OMS validates the bootstrap token against the manufacturing record, MAC, device
class, claim-code hash, and expiry before signing the CSR. If enrollment fails
(network, OMS down, expired token, MAC/class mismatch), the device keeps
running locally and retries on the next boot. There is no infinite retry loop in
`setup()` to keep boot fast.

### Operator owner-claim flow

1. Operator scans the QR label or enters the MAC and claim code in OMS.
2. OMS authenticates the operator, verifies the claim code against the
   manufacturing record, and checks that the device is enrolled but unowned.
3. OMS binds the device to the selected organization/site/asset, marks the
   claim code consumed, and records actor, timestamp, MAC, device id, and
   manufacturing record ID.
4. OMS pushes any site-specific MQTT policy/config over the device config topic;
   the device does not need to expose the claim code again after this point.

### Unclaim flow

Use unclaim when a working device should move to a different owner without
wiping hardware identity. OMS should:

1. Require an authenticated operator with rights on the current owner.
2. Remove owner/site/asset bindings and revoke site-specific ACLs.
3. Keep the device certificate valid only for the quarantine/unclaimed policy,
   or issue a replacement certificate with restricted topics.
4. Mint a new one-time claim code and update the support/claim record; apply a
   new physical label if the old claim code was exposed.

### Fleet lifecycle states after deployment

OMS owns the authoritative lifecycle state, but firmware mirrors terminal or
identity-changing transitions locally so a recovered device cannot keep using
stale credentials. The post-deployment states are:

| State | Meaning | Device behavior | OMS requirements |
|-------|---------|-----------------|------------------|
| `active` | Claimed or claimable device with a valid client certificate and MQTT policy | Normal telemetry, OTA, command, and photo/lock behavior | Maintain current owner/site/asset bindings, certificate status, MQTT ACLs, and OTA target eligibility |
| `transfer_pending` | Current owner is releasing the device to another owner/site | Continue reporting, but OMS should restrict commands to transfer-safe operations | Record releasing actor, new/rotated claim code, old/new owner intent, and any ACL quarantine |
| `transferred` | Device has moved to the new owner/site/asset | Use existing cert only if OMS policy allows the new site, otherwise accept `reprovision` | Audit old owner, new owner, actor, timestamp, MAC/device id, claim code rotation, and policy changes |
| `retired` | Device is scrapped or permanently removed while still reachable | On signed `retire`, publish final retained offline state, erase device identity credentials, keep WiFi/bootstrap for possible bench diagnostics, and reboot unprovisioned | Revoke certificate, MQTT ACLs, bootstrap tokens, claim codes, and OTA targeting; retain audit evidence permanently |
| `decommissioned` | Device has been intentionally taken out of service and should not rejoin without refurbish | Same firmware action as `retire`, with OMS recording the softer business reason | Same revocation/audit as retire; include disposition and expected storage/scrap location |
| `lost_stolen` | Device is missing or suspected hostile | If online, accept only signed lifecycle/remediation commands, publish final retained `lost_stolen`/offline state if possible, then erase identity | Immediately revoke cert/ACLs/tokens/claim codes, disable OTA, alert operators, and require incident audit fields |
| `factory_reset` | Device is being refurbished and all local credentials should be removed | On signed `factory_reset`, publish final retained offline state, erase device cert/key/topics/command key/bootstrap override and WiFi profiles, reset WiFi driver creds, and reboot into captive portal/enrollment | Revoke old cert, mint a fresh bootstrap token/claim code only after physical custody is verified, and link old/new credential lineage |
| `reprovisioning` | Device should keep local WiFi but obtain a fresh OMS identity | On signed `reprovision`, publish final retained offline state, erase device cert/key/topics/command key, retain WiFi and `prov_tok`, and reboot into enrollment | Revoke old cert after final state/ack when possible, issue a short-lived bootstrap token, and audit reason/actor |

Firmware commands are verbs (`retire`, `factory_reset`, `reprovision`); OMS may
map business states such as `decommissioned` or `lost_stolen` to `retire` when
the desired on-device result is identity wipe plus offline retention.

### Signed lifecycle commands

The per-device command topic (`forgekey/<mac>/command`) now accepts these
lifecycle-changing commands after the same signed command envelope validation
used by other OMS commands:

```json
{
  "cmd": "reprovision",
  "command_id": "oms-command-uuid",
  "issued_at": "2026-05-31T12:00:00Z",
  "expires_at": "2026-05-31T12:05:00Z",
  "nonce": "single-use-random",
  "actor": "oms-user-or-service-id",
  "signature": "base64url-raw-es256-signature"
}
```

`retire`, `factory_reset`, and `reprovision` require a non-empty `actor`,
`command_id`, `nonce`, validity window, and either a detached signature over the
canonical envelope or a signed JWT. Devices reject stale, future-dated, replayed,
wrong-device, or unsigned lifecycle commands before any credentials are erased.

Before wiping credentials, firmware best-effort publishes a retained final state
on `forgekey/<mac>/state` with `online:false`, `lifecycle_state`, `reason`,
`command_id`, `actor`, and clock fields. OMS should wait for this retained state
or command ack when the device is online, then revoke credentials. If the device
is already offline, OMS must revoke immediately and mark the final-state publish
as not observed in the audit record.

### Transfer / unclaim flow

Use transfer when a working device should move to a different owner without
necessarily wiping hardware identity. OMS should:

1. Require authenticated approval from the releasing owner and, when known, the
   receiving owner/site administrator.
2. Put the device in `transfer_pending`, remove site-specific ACLs, and rotate
   the human claim code.
3. Decide whether the current client certificate may survive in a quarantine
   policy. If not, send signed `reprovision` while the device is still online.
4. Record old owner/site/asset, new owner/site/asset, actor(s), timestamps, MAC,
   device id, manufacturing record ID, claim-code rotation, and whether a final
   retained offline/reprovisioning state was observed.
5. Complete to `transferred` only after the new owner claims the rotated code
   and the device reports under the new policy.

### Retire, decommission, and lost/stolen flows

Use these states when a device must not continue operating under its current
identity:

1. OMS records the requested terminal state (`retired`, `decommissioned`, or
   `lost_stolen`) with actor, reason, timestamp, MAC, device id, owner/site,
   manufacturing record ID, and evidence/ticket reference.
2. If the device is online and still trusted enough to receive commands, OMS
   sends signed `retire` and waits briefly for the command ack or retained final
   offline/decommissioned state.
3. Firmware erases `dev_id`, MQTT topics, client certificate, private key,
   command public key, broker policy, and lock `asset_id` where present. WiFi
   credentials and `prov_tok` are retained so a bench can still inspect or
   recover the unit without exposing the retired device identity.
4. OMS revokes the client certificate, MQTT ACLs, pending bootstrap tokens,
   unused claim codes, OTA targeting, photo upload authorization, and command
   eligibility. For lost/stolen devices, revocation happens immediately even if
   the final state was not observed.
5. A retired/decommissioned/lost-stolen device may re-enter service only after
   physical inspection, new audit disposition, and explicit creation or reset of
   the manufacturing/bootstrap record.

### Factory-reset and reprovision flows

A signed `factory_reset` is for refurbishing a device that should forget local
network and bootstrap state. A signed `reprovision` is a lighter identity reset
that keeps WiFi and the current NVS bootstrap token override.

1. Operator initiates the command in OMS; OMS signs the command envelope with a
   short expiry and unique nonce.
2. Firmware publishes the final retained offline state while the old certificate
   still works, then wipes local NVS credentials:
   - `reprovision` / `retire`: erase `dev_id`, MQTT topics, client certificate,
     private key, command public key, broker policy, and lock `asset_id`; retain
     `boots`, WiFi profiles, and `prov_tok`.
   - `factory_reset`: erase the same identity keys plus `prov_tok` and saved
     WiFi credentials/profiles, then reset ESP WiFi driver credentials.
3. OMS revokes the old certificate and mints any replacement bootstrap token or
   claim code only after the reset is authorized.
4. Device reboots. `reprovision` can enroll immediately using retained WiFi and
   active bootstrap token. `factory_reset` starts the captive portal before
   enrollment.
5. OMS links old and new credential IDs, CSR fingerprints, command ids, and
   observed final-state/ack telemetry in the audit log.

### Re-enrollment trigger

If an authenticated OMS path returns `401 Unauthorized`, the device clears its
NVS credentials. The next reboot re-runs enrollment with the active bootstrap
token. Mid-loop re-enrollment is out of scope for v1.

## Periodic photo upload

The main loop checks `PhotoUploader::shouldUpload()` each tick:

- True if the last upload was ≥ `PHOTO_UPLOAD_INTERVAL_MS` ago **and** motion
  has been observed within the last `PHOTO_UPLOAD_MOTION_WINDOW_MS`.
- The very first post-boot upload is unconditional so OMS sees the device
  immediately after enrollment.
- Any frame that the detector flags as `motionDetected` or that contains a
  detected person counts as motion.

When eligible:

- Capture a fresh JPEG.
- POST `multipart/form-data` (single `photo` part) to
  `https://OMS_HOST/api/forgekey/devices/<mac>/photo/` over TLS using the
  enrolled client certificate/private key for mTLS.
- Status codes:
  * `2xx` → record `lastUploadMs`, free buffer, done.
  * `401` → clear NVS credentials (forces re-enrollment on next reboot).
  * `5xx` / other → log + continue; next interval will retry.

Photos are JPEG-encoded from the same QVGA grayscale frame the detector uses,
which keeps memory pressure low (one camera config, one PSRAM frame buffer).

## Time requirements and clock health

ForgeKey devices have no trusted battery-backed wall clock. After WiFi is up,
firmware starts SNTP in UTC (`pool.ntp.org`, then `time.nist.gov`) through the
shared time module:

- Arduino/PlatformIO builds use `src/time/time_sync.{h,cpp}`.
- The ESP32-C6 lock build uses `esp32c6-lock/main/forgekey_time.{h,c}`.

The module exposes separate wall-clock and monotonic values so callers do not mix clock domains:

| Field | Meaning | Use |
|-------|---------|-----|
| `clock_valid` | `true` only when the epoch is plausible and the last NTP sync is within the allowed age window (default 24h) | Gate wall-clock security decisions |
| `ntp_synced` | SNTP has set the system clock at least once this boot | Diagnose network/NTP reachability |
| `epoch_time` | Timezone-neutral Unix epoch seconds from `time()` | Timestamps, JWT `exp`/`iat`, TLS validity |
| `last_ntp_sync_age_s` | Monotonic age of the last SNTP sync | Detect stale wall-clock state |
| `uptime_ms` | Monotonic uptime from `millis()`/`esp_timer_get_time()` | Durations, backoff, debounce, local scheduling |

Status, health, diagnostics, OTA status, device state, telemetry, and command
ack payloads include these clock fields. Operators should treat `timestamp` or
`epoch_time` as UTC epoch seconds only when `clock_valid` is `true`; `uptime_ms`
remains valid for elapsed-time math regardless of wall-clock sync.

Signed command envelopes that require wall-clock checks (`issued_at`,
`expires_at`, or JWT `exp`) are rejected with `clock_invalid` until the clock is
valid. The only exception is a server-provided challenge/nonce flow (`auth_flow:
"challenge"`/`"nonce"`, `challenge`, or `server_nonce` present), where freshness
comes from the signed one-time challenge plus the device replay cache instead
of the device wall clock.


## Watchdog and unattended-reliability expectations

Every ForgeKey firmware target is expected to remain unattended for long field
runs. The watchdog policy is therefore layered rather than a single immediate
reboot trigger:

1. **Task watchdog:** When the underlying ESP framework exposes
   `esp_task_wdt`, the main application task is registered with a 30 second
   task watchdog. The firmware feeds that watchdog from the main loop only
   while no unsafe subsystem has exceeded its health deadline.
2. **Subsystem health timers:** Firmware tracks independent monotonic health
   timers for WiFi, MQTT, camera, OTA, BLE, sensors, and the lock state
   machine. A subsystem marks itself healthy when it completes normal loop
   work, has an active connection, observes sensor/state-machine progress, or
   is intentionally busy on a long-running operation.
3. **Local recovery before reboot:** On a health timeout the device first tries
   the narrowest safe recovery: WiFi disconnect/reconnect for WiFi, MQTT client
   restart for MQTT, and capability/state-machine grace windows for camera,
   BLE, and sensor work. Only if a subsystem remains unhealthy after repeated
   warnings does the firmware escalate to a whole-device restart.
4. **Operator-visible telemetry:** Watchdog warnings are published to the
   device status topic as `{"event":"watchdog_warning",...}` when MQTT is
   available, and normal health/status telemetry includes a `watchdog` object
   with task-WDT enablement, last reset reason, safe-guard state, per-subsystem
   ages, warning counts, and recovery counts.
5. **Safe guards for critical sections:** OTA flash writes, OTA metadata/NVS
   writes, other NVS erase/write paths, and lock solenoid actuation suspend
   aggressive watchdog escalation while they are in progress. The task watchdog
   is still fed during these guarded windows so a legitimate long flash write or
   lock pulse is not interrupted mid-operation. Once the critical section exits,
   subsystem timers are refreshed and normal timeout escalation resumes.

The intended operations posture is: warnings should be investigated as soon as
OMS shows them, but a single transient radio/broker/sensor stall should recover
without human intervention and without disturbing a safe lock or OTA state.

## OTA firmware updates

OTA is dispatched over MQTT, not pulled. After enrollment the device
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

### Status reporting

Throughout `apply()` the device publishes progress JSON to a status topic so
OMS can render an OTA progress UI in real time. The status topic defaults to
the dispatch topic plus `/status` (e.g. `forgekey/<mac>/people_counter/firmware/status`)
and can be overridden via `MqttClient::setFirmwareStatusTopic()`.

Payload shape (clock fields are included on every OTA status):

```json
{
  "state": "downloading",
  "version": "0.2.0",
  "progress": 60,
  "clock_valid": true,
  "ntp_synced": true,
  "epoch_time": 1715150000,
  "last_ntp_sync_age_s": 12,
  "uptime_ms": 12345678,
  "ts": 1715150000
}
```

Lifecycle states, in order:

| State | When | `progress` | `error` |
|-------|------|-----------:|---------|
| `received` | Dispatch parsed successfully | — | — |
| `deferred` | Non-mandatory update postponed (recent photo upload) | — | `device_busy` |
| `downloading` | Streaming the body — emitted at start and each ~10% boundary | `0..100` | — |
| `verifying` | Body fully downloaded, computing SHA-256 + ECDSA verify | `100` | — |
| `rebooting` | Both checks passed, `Update.end()` succeeded — about to `ESP.restart()` | `100` | — |
| `applied` | First successful occupancy publish post-reboot, partition blessed via `markStableIfPending()` | `100` | — |
| `failed` | Any failure path | — | one of: `parse_error`, `malformed_url`, `connect_failed`, `http_<code>`, `no_content_length`, `update_begin_failed`, `read_timeout`, `connection_closed`, `flash_write_failed`, `sha256_mismatch`, `bad_base64_signature`, `signature_invalid`, `update_end_failed` |

Status publishing is best-effort: if MQTT is offline at the moment of an
event the publish silently drops, but the OTA download itself never blocks
on the status path.

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

Production builds open this namespace from the encrypted production NVS
partition defined in `partitions/forgekey_secure_ota.csv`; development builds
continue to use the default development NVS partition. See
`docs/SECURITY_BASELINE.md` for the manufacturing and key-handling baseline.

| Key | Type | Description |
|-----|------|-------------|
| `boots` | uint32 | Persistent boot counter |
| `dev_id` | str | OMS-assigned device id |
| `fw_topic` | str | OTA dispatch MQTT topic |
| `p_topic` | str | Occupancy/status publish MQTT topic |
| `c_cert` | str | Per-device MQTT/HTTPS client certificate issued by OMS |
| `c_key` | str | Per-device private key generated on-device during enrollment |
| `cmd_pub` | str | OMS command public key/cache for signed device commands |
| `b_host` | str | OMS-assigned MQTT broker hostname |
| `b_port` | uint16 | OMS-assigned MQTT broker TLS port |
| `b_tls` | bool | Whether the MQTT broker policy requires TLS |
| `prov_tok` | str | Short-lived/per-device bootstrap token override (overrides compile-time default) |

Field lifecycle commands call `provisioning.clear()` or the lower-level
credential wipe helper. `clear()` deliberately preserves `boots` and `prov_tok`
so a freshly issued bootstrap token survives `reprovision`; signed
`factory_reset` additionally removes `prov_tok` and saved WiFi profiles so the
unit returns to captive-portal onboarding.

Additional namespaces used by lifecycle support:

| Namespace | Keys | Description |
|-----------|------|-------------|
| `fk_lifecycle` | `state` | Last local lifecycle transition such as `retired`, `reprovisioning`, or `factory_reset` for bench diagnostics after reboot. |
| `fk_wifi` / `wifi_creds` | build-specific WiFi profile keys | Saved WiFi credentials; retained for `retire`/`reprovision`, wiped for `factory_reset`. |

## Bootstrap token rotation

Devices subscribe to `forgekey/<mac>/config`. A payload of:

```json
{ "provisioning_token": "<new>", "valid_after": "2026-05-01T00:00:00Z" }
```

writes the new short-lived/per-device bootstrap token to NVS (`prov_tok`). The
device uses that token on its next re-enrollment. OMS should mint tokens scoped
to one MAC/device class/manufacturing record and should expire unused tokens
quickly. `valid_after` is parsed and logged but the device does not enforce it
— the back-end controls when tokens are accepted by
`/api/forgekey/devices/enroll/`.

## Security model

- **Transport**: TLS to OMS with the OMS root certificate baked into firmware
  (`src/security/oms_ca.h`). `WiFiClientSecure::setCACert()` is used in all
  three HTTPS paths (enrollment, photo upload, OTA download). MITM via a
  rogue public CA is rejected at the TLS handshake.
- **Bootstrap bearer**: short-lived/per-device token provided as
  `FORGEKEY_BOOTSTRAP_TOKEN` at manufacturing time or delivered into NVS as
  `prov_tok`. It is sent in `X-ForgeKey-Bootstrap-Token` only during
  enrollment and is validated against MAC, device class, claim code,
  manufacturing record, and expiry.
- **Per-device auth**: each device generates its own P-256 private key and CSR.
  OMS signs the CSR and returns a unique client certificate used for MQTT/HTTPS
  authentication thereafter.
- **Signed lifecycle commands**: `retire`, `factory_reset`, and
  `reprovision` use the OMS command public key, a unique `command_id`/`nonce`,
  `actor`, and `issued_at`/`expires_at` envelope. Firmware rejects replayed or
  expired commands and publishes a retained final offline lifecycle state before
  wiping credentials when it still has MQTT access.
- **OMS revocation and audit**: OMS must revoke the old client certificate, MQTT
  ACLs, bootstrap tokens, claim codes, OTA targeting, upload permissions, and
  command eligibility for terminal or identity-reset states. Audit records must
  include actor, reason, command id, timestamps, MAC, device id, manufacturing
  record, owner/site/asset, credential serial/fingerprint, revocation time, and
  whether final retained state/ack was observed.
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

### Bootstrap token

For rotation or refurbish:

1. Generate a new token scoped to the device MAC, class, manufacturing record,
   and a short expiry; generate a fresh claim code if ownership can change.
2. Publish the rotation message to the device's `forgekey/<mac>/config` topic
   while it is online, or inject the token in NVS during refurbish. The device
   persists it to `prov_tok`.
3. Revoke any previous unused bootstrap token and old claim code in OMS.
4. Reboot or factory-reset the device so it posts a new CSR/public key to
   `/api/forgekey/devices/enroll/`.

## Manual smoke test (hardware)

After `~/.platformio/penv/bin/platformio run --target upload`:

1. Watch the serial monitor — confirm WiFi connect and `provision: first
   boot — enrolling with OMS`.
2. Confirm the enrollment POST appears in OMS access logs and the device
   shows up in the OMS admin device list.
3. Within 5 minutes of motion, confirm a photo lands in the OMS device
   gallery for that MAC.
4. From OMS admin, dispatch a fake firmware (any signed `firmware.bin`) on
   `forgekey/<mac>/firmware`. Confirm the serial log shows
   `ota: downloading → installed → rebooting`.
5. After reboot, confirm the new firmware version appears in the next
   enrollment/status ping or occupancy log line, and that
   `ota: marked running partition as valid` appears once a publish lands.

## Building & releasing firmware

`scripts/build/version.py` is registered as a PlatformIO `pre:` extra script
in `platformio.ini`. It runs at every build and:

1. **Injects build identity macros** into the compile so the binary knows
   exactly which version it is:
   - `FORGEKEY_FIRMWARE_VERSION` — read from `src/provisioning/device_config.h`
     (or override via env: `FORGEKEY_FIRMWARE_VERSION=0.2.0 pio run`)
   - `FIRMWARE_GIT_COMMIT` — short SHA from `git rev-parse`, with `-dirty`
     suffix when the working tree has uncommitted changes
   - `FIRMWARE_BUILD_TIMESTAMP` — Unix epoch at build time. Honors
     `SOURCE_DATE_EPOCH` when set so CI can produce reproducible builds.
2. **Exports the artifact** after `firmware.bin` is produced. The merged
   binary is copied to (variant slug derived from the PlatformIO env so
   the two device kinds never overwrite each other):

   ```
   artifacts/
     forgekey-<variant>-<version>-<commit>.bin
     forgekey-<variant>-<version>-<commit>.bin.sha256
     forgekey-<variant>-latest.bin
     forgekey-<variant>-latest.bin.sha256
   ```

   `<variant>` is one of `people-counter` or `temperature-sensor`.

   The `.sha256` file holds plain hex (lowercase, no filename suffix) — the
   exact shape OMS pastes into the OTA dispatch payload's `sha256` field.

### Cutting a release

```bash
# 1. Bump the version in src/provisioning/device_config.h, commit it.
# 2. Build:
~/.platformio/penv/bin/platformio run

# 3. Sign the resulting artifact:
openssl dgst -sha256 -sign .firmware-keys/firmware-signing.pem \
    -out artifacts/forgekey-<version>-<commit>.bin.sig \
    artifacts/forgekey-<version>-<commit>.bin
base64 -w0 artifacts/forgekey-<version>-<commit>.bin.sig

# 4. Upload artifacts/forgekey-<version>-<commit>.bin to OMS static hosting,
#    then dispatch via the OMS admin OTA form using the .sha256 contents and
#    the base64-encoded signature.
```

`artifacts/` is gitignored — it's a build output, not source.

## Temperature-sensor variant

A second device kind ships from this firmware tree as a separate PlatformIO
build env (`seeed_xiao_esp32s3_temperature`). Same skeleton — captive portal,
provisioning, OTA, MQTT, config rotation — but the camera/detection/photo
pipeline is compiled out and a DHT 21 (AM2301) is sampled instead.

```bash
~/.platformio/penv/bin/platformio run -e seeed_xiao_esp32s3_temperature
```

| Aspect | People-counter | Temperature-sensor |
|---|---|---|
| Sensor kind sent at enrollment | `people-counter` | `temperature-sensor` |
| MQTT topic kind segment | `people_counter` | `temperature_sensor` |
| Publish topic leaf | `/occupancy` | `/reading` |
| Publish payload | `{count, timestamp, clock_valid, ntp_synced, epoch_time, uptime_ms, last_ntp_sync_age_s}` | `{tempC, humidity, timestamp, clock_valid, ntp_synced, epoch_time, uptime_ms, last_ntp_sync_age_s}` |
| Publish cadence | on-change + ≤10s | every 30s (`TEMPERATURE_SAMPLE_INTERVAL_MS`) |
| Hardware | XIAO ESP32-S3 Sense (camera) | XIAO ESP32-S3 + DHT21 on GPIO 2 |
| Photo upload | yes | n/a |
| Registration multipart | metadata + photo | metadata only |

The DHT data pin defaults to GPIO 2 (D1 on the XIAO header) and is
overridable via `-DFORGEKEY_DHT_PIN=<pin>`. A 4.7k–10k pull-up between the
data line and 3V3 is recommended on long leads. The sample interval is
overridable via `-DTEMPERATURE_SAMPLE_INTERVAL_MS=<ms>`.

The OTA dispatch flow is shared, but the firmware images are NOT
interchangeable — a temperature-sensor device cannot run a people-counter
binary and vice-versa. The build script keys artifact names on the variant
(`forgekey-temperature-sensor-<ver>-<sha>.bin`) so OMS dispatchers can target
the right binary per device kind.

## Out of scope (filed as follow-ups)

- Offline queue + exponential backoff for unreachable OMS
- Multi-channel update streams (stable / beta / dev)
- ESP32 hardware Secure Boot v2 (fuse-burning, irreversible)
- Full server-side claim/retire audit UI and label reprint automation
