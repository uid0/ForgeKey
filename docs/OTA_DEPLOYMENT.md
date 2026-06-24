# OTA Deployment Workflow

End-to-end walkthrough for shipping a new firmware image to ForgeKey
devices via OpenMakerSpace (OMS). This is the **operator** view — what to
type, in what order, and where the bytes go. For the device-side OTA
internals (apply flow, rollback, status payloads, signature verification),
see [FORGEKEY_DEVICE.md "OTA firmware updates"](../FORGEKEY_DEVICE.md#ota-firmware-updates).

The sections below are cumulative: do (1) once per fleet, then (2)–(5)
each release. Before dispatching a release to production, complete the build,
schema, documentation, and HIL rungs in [VERIFICATION_LADDER.md](VERIFICATION_LADDER.md).

- [1. One-time signing-key setup](#1-one-time-signing-key-setup)
- [2. Build a deployable binary](#2-build-a-deployable-binary)
- [3. Upload and dispatch via OMS](#3-upload-and-dispatch-via-oms)
- [4. Fleet rollout controls](#4-fleet-rollout-controls)
- [5. Troubleshooting](#5-troubleshooting)

---

## 1. One-time signing-key setup

ForgeKey OTA images are signed with **ECDSA P-256**. The device verifies
each dispatch against a public key compiled into firmware. Arduino targets
read that key from `src/security/firmware_pubkey.h`; the ESP32-C6 cabinet-lock
target carries a synced copy in `esp32c6-lock/main/firmware_pubkey.h`. The
matching private key lives only on the OMS server, exported as the
`FORGEKEY_FIRMWARE_SIGNING_KEY` env var.

You only do this once per fleet — every device flashed with the same
`firmware_pubkey.h` will accept any image signed by the matching private
key, regardless of version.

### 1.1 Generate the keypair

```bash
# In the forgekey repo root:
scripts/build/gen-firmware-signing-key.sh
```

Output (writes to `.firmware-keys/`, which is **gitignored**):

```
.firmware-keys/firmware-signing.key   # private (PKCS8 PEM, mode 0600)
.firmware-keys/firmware-signing.pub   # public  (PEM)
```

The script refuses to overwrite an existing `firmware-signing.key` — if you
truly want to rotate, move the old one aside first (see
[Key rotation](#15-key-rotation) below).

### 1.2 Bake the public key into the firmware

```bash
scripts/build/gen-firmware-signing-key.sh --update-header
```

This rewrites `src/security/firmware_pubkey.h` in place from
`.firmware-keys/firmware-signing.pub`. **Commit the updated header.** Every
device flashed from that point forward will trust signatures from the
matching private key.

If you ship the ESP32-C6 cabinet-lock firmware, also sync the generated lock
copy before building:

```bash
python3 esp32c6-lock/scripts/esp32c6-sync-security.py
```

```bash
git add src/security/firmware_pubkey.h
git commit -m "chore(security): rotate firmware signing public key"
```

> ⚠️ A device flashed with the placeholder `firmware_pubkey.h` (the
> committed default) will **reject all signatures**. You must replace it
> before any device can accept an OTA.

### 1.3 Deploy the private key to OMS

The OMS backend signs OTA dispatches automatically using the private key
loaded from the `FORGEKEY_FIRMWARE_SIGNING_KEY` env var. The value is the
**full multiline PEM**, including the `BEGIN/END EC PRIVATE KEY` lines.

For a typical Docker/Compose deployment, set it in OMS's `.env`:

```bash
# Example .env entry — note the literal newlines preserved with $'...' or
# a heredoc; the OMS env loader accepts the raw multiline PEM.
FORGEKEY_FIRMWARE_SIGNING_KEY="$(cat .firmware-keys/firmware-signing.key)"
```

Then restart the OMS web + Celery workers so the new value is picked up.

### 1.4 Back the private key up

If you lose `firmware-signing.key` and the OMS env var is wiped, **the
fleet is bricked for OTA** — no future image can satisfy the signature
check baked into already-deployed devices. Recovery requires physically
re-flashing every device with a new `firmware_pubkey.h`.

Mitigations:

- Store an offline copy in a password manager / hardware token / sealed
  envelope. Treat it like a root CA private key.
- Keep at least one off-site copy in a separate trust domain from the
  primary OMS host.
- Restrict access — anyone with the private key can ship arbitrary
  firmware to every device on the fleet.

### 1.5 Key rotation

Rotation procedure (suspected compromise, scheduled hygiene, or staff
turnover). See also [FORGEKEY_DEVICE.md "Firmware-signing keypair"](../FORGEKEY_DEVICE.md#firmware-signing-keypair)
for the device-side view.

The hard part is that already-deployed devices only trust the **old**
public key. There are two viable paths:

**Path A — Dual-trust transition (zero downtime):**

1. Generate the new keypair (move the old `firmware-signing.key` aside
   first so the script doesn't refuse).
2. Edit `src/security/firmware_pubkey.h` so it carries **both** the old
   and new PEM blocks, and teach `firmware_verify::verifySignature()` to
   try both. (Not currently implemented — file a bead before relying on
   this path.)
3. Bump `FORGEKEY_FIRMWARE_VERSION`, build, and dispatch the
   transition build using the **old** signing key.
4. Wait for fleet uptake (every device now trusts both keys).
5. Switch OMS to the new private key, drop the old PEM from the header,
   and ship a final clean-up build signed by the new key.
6. Retire the old private key from OMS and your backups.

**Path B — Dual-publish (simpler, requires per-device targeting):**

1. Generate the new keypair as above.
2. Keep both private keys loaded in OMS (dual env vars, or per-firmware
   selection).
3. For each device, dispatch a transition build signed by the **old** key
   that updates the baked-in `firmware_pubkey.h` to the new one. (Same
   caveat: the firmware needs to support two-key trust during the
   transition build, otherwise you risk bricking devices mid-rollout.)
4. From that point on, dispatch new images signed by the new key.

**Path C — Wipe and re-flash (no software changes):**

If the fleet is small and physically accessible, the simplest rotation is
to USB-flash every device with the new `firmware_pubkey.h`. No transition
build needed.

---

## 2. Build a deployable binary

ForgeKey ships three device variants from one tree:

| Build target | Variant slug | Purpose |
|---|---|---|
| `seeed_xiao_esp32s3` | `people-counter` | Camera-based occupancy counting |
| `seeed_xiao_esp32s3_temperature` | `temperature-sensor` | DHT 21 temperature/humidity |
| `seeed_xiao_esp32s3_asset_indicator` | `indicator` | Asset-mounted RGB matrix status indicator |
| `esp32c6-lock/` (`idf.py build`) | `cabinet-lock` | Cabinet lock on ESP32-C6 / ESP-IDF |
| `seeed_xiao_epaper` | `epaper-display` | Preventive-maintenance e-paper display on XIAO ESP32-C3 |

These binaries are **not interchangeable**. Dispatch the right image for the
target hardware and firmware family.

### 2.1 Bump the version and release metadata

Every release artifact must have a unique `FORGEKEY_BUILD_ID`. The build
metadata generator derives one from target environment, firmware version, git
SHA, dirty flag, build timestamp, release channel, and signing key ID. CI or a
release engineer may override it with `FORGEKEY_BUILD_ID`, but never reuse a
build ID across two different `.bin` artifacts.

Set the release metadata before building production artifacts:

```bash
export FORGEKEY_RELEASE_CHANNEL=<dev|staging|production>
export FORGEKEY_SIGNING_KEY_ID=<oms-signing-key-id>
# Optional only when CI assigns immutable artifact IDs:
# export FORGEKEY_BUILD_ID=<globally-unique-artifact-id>

# Edit src/provisioning/device_config.h, bump FORGEKEY_FIRMWARE_VERSION.
# If shipping the cabinet-lock build, keep esp32c6-lock/main/device_config.h
# in sync before building.
# Commit the bump (the build embeds the git SHA, so commit-then-build keeps
# the artifact name and the embedded version coherent).
git add src/provisioning/device_config.h
git commit -m "chore(release): bump firmware to <version>"
```

### 2.2 Build

```bash
# People-counter variant (default env):
~/.platformio/penv/bin/platformio run

# Temperature-sensor variant:
~/.platformio/penv/bin/platformio run -e seeed_xiao_esp32s3_temperature

# E-paper display variant:
~/.platformio/penv/bin/platformio run -e seeed_xiao_epaper

# Cabinet-lock variant:
cd esp32c6-lock
. $IDF_PATH/export.sh
idf.py build
```

### 2.3 Locate the binary

PlatformIO writes the Arduino build outputs to:

```
.pio/build/<env>/firmware.bin
```

But you almost never need that path directly. The post-build hook
(`scripts/build/version.py`) automatically copies the merged binary to a
versioned artifact path keyed on the variant, version, short git SHA, and unique build ID:

```
artifacts/
  forgekey-people-counter-<version>-<commit>-<build-id>.bin
  forgekey-people-counter-<version>-<commit>-<build-id>.bin.sha256
  forgekey-people-counter-latest.bin
  forgekey-people-counter-latest.bin.sha256

  forgekey-temperature-sensor-<version>-<commit>-<build-id>.bin
  forgekey-temperature-sensor-<version>-<commit>-<build-id>.bin.sha256
  forgekey-temperature-sensor-latest.bin
  forgekey-temperature-sensor-latest.bin.sha256

  forgekey-epaper-display-<version>-<commit>-<build-id>.bin
  forgekey-epaper-display-<version>-<commit>-<build-id>.bin.sha256
  forgekey-epaper-display-latest.bin
  forgekey-epaper-display-latest.bin.sha256
```

The generated firmware also embeds the same build metadata under the `build` JSON object reported by enrollment, capability announcements, state/status snapshots, health/diagnostic reports, and OTA status payloads. The `.sha256` file holds the lowercase hex digest with no filename suffix
— the exact shape OMS pastes into the dispatch payload's `sha256` field.

`artifacts/` is gitignored — these are build outputs, not source.

To verify what you just built:

```bash
ls -la artifacts/
cat artifacts/forgekey-people-counter-latest.bin.sha256
```

If the working tree was dirty at build time the embedded git commit will
have a `-dirty` suffix. **Don't ship `-dirty` artifacts to production.**
Commit first, then rebuild.

For the ESP32-C6 cabinet-lock build, `idf.py build` writes binaries under
`esp32c6-lock/build/`, typically including `lock_device.bin`. Hash those
artifacts directly when preparing an OTA upload:

```bash
shasum -a 256 esp32c6-lock/build/lock_device.bin
```

---

## 3. Upload and dispatch via OMS

OMS handles signing and dispatch; you just need to upload the binary and
target a device (or fleet). The backend signs the artifact with
`FORGEKEY_FIRMWARE_SIGNING_KEY` at upload time, then publishes the
dispatch JSON over MQTT to the matching device(s).

### 3.1 Upload the artifact

In the OMS Django admin (`/admin/`):

1. Navigate to **ForgeKey → Firmware Updates → Add new**.
2. Upload the `.bin` from `artifacts/` (e.g.
   `forgekey-people-counter-0.2.0-abcd123-<build-id>.bin`).
3. Record the embedded **build ID** from the artifact name/CI metadata. It must
   be unique for this binary and must match the `build.id` that devices report
   after installation.
4. Set the **version** field to match the embedded build version
   (semver, e.g. `0.2.0`). OMS uses this to populate the dispatch
   payload's `version` field, which the device echoes back in registration
   pings.
5. Set the **hardware revision tag** if your fleet has multiple revs
   (e.g. `esp32-s3-sense-v1`). OMS uses this when fleet-targeting to
   avoid shipping the wrong binary to incompatible hardware.
6. **Save.** OMS computes the SHA-256, base64-encodes the ECDSA(P-256)
   signature using the private key from
   `FORGEKEY_FIRMWARE_SIGNING_KEY`, and stores both alongside the binary.

### 3.2 Dispatch to a device or fleet

From the firmware-update detail page:

- **Deploy to device**: pick a single device. OMS publishes one dispatch
  message and watches the device's status topic for progress.
- **Deploy to fleet**: targets every device matching the variant +
  hardware revision. OMS fans the dispatch out via Celery — one publish
  per device.

For MQTT-managed devices, the backend Celery task publishes the dispatch JSON to:

```
forgekey/<mac>/<kind>/firmware
```

where `<kind>` is `people_counter`, `temperature_sensor`, or another
MQTT-managed device kind depending on the registered sensor kind. The exact topic was returned to the
device at registration in the `mqtt_topic_for_firmware` field of the
register response, persisted to NVS as `fw_topic`, and is the topic the
device subscribes to on every boot.

The dispatch payload shape is shared by Arduino MQTT devices,
`seeed_xiao_epaper` HTTPS polling, and the ESP-IDF cabinet-lock firmware:

```json
{
  "url": "https://oms.openmakerspace.org/static/firmware/<filename>.bin",
  "sha256": "f3b5...64hex",
  "signature": "MEUCIQ...base64-DER-ECDSA(P-256)",
  "version": "0.2.0",
  "mandatory": false,
  "policy": {
    "minimum_version": "0.1.0",
    "maximum_version": "0.1.99",
    "hardware_target": "seeed_xiao_epaper",
    "capability_target": "epaper_pm",
    "rollout_cohort": "pct:10",
    "deadline": "1767225600"
  }
}
```

`policy` may be omitted, and policy fields may also be sent at the top
level for older OMS serializers. Devices reject a dispatch before download
when their current version, hardware target, active capability, or rollout
cohort does not match. `minimum_version` and `maximum_version` describe the
source-version compatibility window for devices that may safely apply this
image; they are not the target image version. `mandatory=true` makes the
device apply immediately even if deferrable work is in flight.
`mandatory=false` defers the apply if the device uploaded a photo in the
last 5 seconds. `deadline` should be epoch seconds when device-side
wall-clock enforcement is required.

E-paper displays do not stay connected to MQTT. On each wake they poll:

```
GET  /api/forgekey/epaper/<display_id>/firmware.json
POST /api/forgekey/epaper/<display_id>/firmware/status/
```

A `200` response to `firmware.json` must be the same signed dispatch JSON
shown above; `204` or `404` means no update. The display posts the same OTA
lifecycle statuses as MQTT devices before proceeding to its image fetch.

### 3.3 Watch progress

The device publishes lifecycle events to:

```
forgekey/<mac>/<kind>/firmware/status
```

(i.e. the dispatch topic with `/status` appended.) OMS subscribes to this
and renders progress in the admin UI in real time. State transitions:

```
received → downloading (0..100%) → verifying → rebooting → applied
```

Rejected policy dispatches publish `state=rejected` with `error` set to one
of `below_minimum_version`, `above_maximum_version`,
`hardware_target_mismatch`, `capability_target_mismatch`, or
`rollout_cohort_mismatch`. Health/status payloads include `ota_partition`,
`ota_running_slot`, `ota_boot_slot`, `ota_next_slot`, `ota_state`,
`ota_pending_verify`, `ota_previous_version`, `ota_secure_version`, and
anti-rollback support/check fields so OMS can gate rollouts on actual slot
state.

`applied` only fires after the new firmware reboots, reconnects to MQTT,
and publishes its first occupancy/reading. Until then the partition is
marked `ESP_OTA_IMG_PENDING_VERIFY` and a power cycle will roll back to
the previous image. This is intentional — it's the device's last line of
defence against a bricking image. See
[FORGEKEY_DEVICE.md "Rollback safety"](../FORGEKEY_DEVICE.md#rollback-safety).

### 3.4 Confirm the device is on the new build

After `applied`, the next registration ping (or the device's serial log)
will report the new `firmware_version`. Watch the OMS device detail page
or tail the device's serial monitor:

```bash
~/.platformio/penv/bin/platformio device monitor
```

Look for log lines like `ota: marked running partition as valid`.

---

## 4. Fleet rollout controls

Fleet OTA is deliberately staged. Operators should create a rollout record
in OMS with immutable artifact metadata (version, SHA-256, signature,
hardware target, capability target) and mutable rollout state (cohort,
percentage, health gate results, paused/aborted flag).

### 4.1 Cohorts and canaries

Use these cohorts for every production release:

1. **Lab canary**: 1-3 bench devices per hardware target. Must include at
   least one device already on the oldest supported `minimum_version`.
2. **Staff canary**: 1-5% of real devices owned by staff/operators who can
   power-cycle or USB-flash quickly.
3. **Site canary**: one device per site/network segment, especially where
   captive portals, firewalls, or weak RSSI are common.
4. **Production staged rollout**: deterministic percentage cohorts
   (`pct:10`, `pct:25`, `pct:50`, `pct:100`) based on device identity so a
   device does not jump between cohorts during a retry.

Never skip directly from lab to 100% unless the release is an emergency
security update and rollback has already been exercised on the same target.

### 4.2 Staged percentage rollout

Recommended production cadence:

| Stage | Target policy | Minimum dwell time | Promotion requirement |
|---|---|---:|---|
| Lab | explicit device list | 30 minutes | All devices report `applied`, stable slot valid |
| 1% | `rollout_cohort=pct:1` | 2 hours | Health gates green |
| 10% | `rollout_cohort=pct:10` | 4 hours | Health gates green, no site-level cluster |
| 25% | `rollout_cohort=pct:25` | 8 hours | Health gates green |
| 50% | `rollout_cohort=pct:50` | 12 hours | Health gates green |
| 100% | `rollout_cohort=pct:100` | release-specific | Monitor until long-tail complete |

Mandatory updates may shorten dwell time only with incident commander
approval. Deadline-based policies should set `mandatory=true` once the
maintenance deadline passes, but still keep hardware/capability filters.

### 4.3 Health gates

Pause promotion if any gate fails within the current stage:

- **Download/apply success**: at least 98% of contacted devices reach
  `rebooting`; at least 95% reach `applied` within the dwell window.
- **Rollback safety**: no more than 1% report `ota_pending_verify=true` for
  more than 15 minutes after reboot, and no device repeatedly alternates
  between previous and target versions.
- **Runtime health**: no statistically significant increase in MQTT
  disconnects, boot loops, lock alarm state, failed image fetches, missing
  temperature readings, or people-counter capture failures.
- **Partition health**: devices must report expected `ota_running_slot`, a
  non-empty `ota_partition`, and a valid anti-rollback check when the target
  supports secure-version fuses.
- **Site health**: no site/network segment has more than two devices failing
  with the same reason (`connect_failed`, `http_404`, `signature_invalid`,
  etc.).

### 4.4 Abort criteria

Abort the rollout immediately and stop dispatching when any of these occur:

- Any confirmed bricked device that cannot enter the old slot after a power
  cycle.
- `signature_invalid`, `sha256_mismatch`, or `anti_rollback_rejected` on
  more than one device; these indicate artifact/signing metadata problems.
- More than 2% boot-looping, rolling back, or failing to reconnect to MQTT
  after the update.
- Any cabinet-lock release that increases unsafe/unsecured state reports,
  unlock failures, or alarm timeouts above the pre-rollout baseline.
- Any e-paper release that prevents panels from deep-sleeping or fetching
  display content after OTA.

An aborted rollout must leave the OMS rollout record paused with the final
abort reason, affected cohorts, first/last failure timestamps, and links to
serial/MQTT logs.

### 4.5 Rollback policy

ForgeKey uses ESP-IDF/Arduino OTA rollback as the first safety layer: a new
slot remains pending until the firmware reconnects and marks itself valid.
Operational rollback is separate:

1. If the new image is merely unhealthy but devices still accept OTA,
   dispatch the previous known-good version with a higher compatible
   anti-rollback secure version (where secure-version fuses are enabled).
2. If anti-rollback fuses prevent reinstalling the exact old image, build a
   hotfix from the old source with a bumped secure version and clear release
   notes that it is a rollback hotfix.
3. If devices cannot reach OTA but boot the previous slot after power-cycle,
   abort and wait for automatic/manual rollback rather than repeatedly
   redispatching.
4. If neither slot boots, recover by USB flashing and record the device as a
   bricking incident.

Keep every release artifact, SHA-256, signature, and signing-key identifier
until the fleet has moved beyond the release and the rollback window has
expired.

### 4.6 Compatibility rules

- `hardware_target` must name the exact build family (`seeed_xiao_esp32s3`,
  `seeed_xiao_esp32s3_temperature`, `seeed_xiao_epaper`, or
  `esp32c6-lock`). Do not use `all` for production except emergency signing
  key transition images that are known to be cross-compatible.
- `capability_target` must match the active capability (`people_counter`,
  `temperature_sensor`, `epaper_pm`, `cabinet_lock`, etc.) or the registered
  sensor kind. A device rejecting a capability mismatch is behaving
  correctly.
- `minimum_version`/`maximum_version` must bracket the versions whose NVS
  layout, MQTT topics, and partition table are compatible with the target
  image. Split the rollout into multiple bridge releases if NVS or partition
  layout changes are not backwards-compatible.
- Never change the partition table for an OTA-only release unless every
  currently deployed partition table can receive and boot the new image.
- Anti-rollback secure-version values must be monotonically non-decreasing.
  Once an eFuse secure version is advanced, images with lower secure
  versions cannot be used for rollback on that device class.

---

## 5. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Device logs `signature_invalid` and aborts | The committed `firmware_pubkey.h` doesn't match the private key OMS used to sign. Either the device was flashed before the rotation, or OMS is using a stale `FORGEKEY_FIRMWARE_SIGNING_KEY`. | Confirm `git log src/security/firmware_pubkey.h` matches the key OMS holds. Re-flash the device or update the OMS env var to match the deployed pubkey. |
| Device logs `sha256_mismatch` | The binary was modified after OMS computed the digest, or the `sha256` field in the dispatch was wrong. | Re-upload the artifact through OMS so it recomputes both the digest and the signature. Don't hand-edit `.sha256` files. |
| Device logs `connect_failed` or `http_404` on download | Signed download URL expired (default 5-minute TTL), or the static-file host is unreachable from the device's network. | Re-dispatch from OMS to mint a fresh URL. If repeated, check OMS's `STATIC_URL` and that the device can resolve and reach that host. |
| Device logs `no_content_length` | OMS static handler is returning chunked-transfer encoding. The OTA client requires `Content-Length`. | Configure the OMS static-file serving path to return `Content-Length` (typical for nginx/whitenoise; check the deployment's reverse proxy config). |
| Device never receives the dispatch (no log at all) | (a) Device isn't subscribed to the topic OMS is publishing to (provisioning mismatch), or (b) MQTT bridge between OMS Celery and the broker is broken, or (c) device isn't connected to MQTT. | (a) Check `bd show oms-zad` — known `ALLOWED_HOSTS` issue can block registration, leaving the device with no `fw_topic`. Check device serial for `MQTT subscribe firmware topic=...`. (b) Check the OMS Celery worker logs for the publish. (c) Check device serial for an active MQTT connection. |
| `gen-firmware-signing-key.sh` refuses to run with "Refusing to overwrite existing $PRIV" | `.firmware-keys/firmware-signing.key` already exists. | If you intend to rotate: `mv .firmware-keys/firmware-signing.key .firmware-keys/firmware-signing.key.old` first, then re-run. **Do not delete the old key** until you've finished the rotation transition. |
| Build artifact name has a `-dirty` suffix | Working tree had uncommitted changes when you built. | Don't ship `-dirty` artifacts to production. `git status` clean, then rebuild. |
| Device boots into the new firmware but never sends `applied`; reverts on reboot | The new firmware is failing to publish on MQTT before the rollback timer (next power cycle). | Check the device serial log for crashes / MQTT connect failures on the new build. The rollback is the safety net working — the new build is broken. Fix and re-dispatch. |
| Two devices keep getting the wrong variant binary | Hardware revision tag isn't set or the dispatch is using "Deploy to fleet" without filtering. | Set the hardware revision tag on each FirmwareUpdate row, and confirm device records have matching `sensor_kind`. People-counter and temperature-sensor binaries are not interchangeable. |

For Dolt / OMS infrastructure issues that aren't device-side, escalate via
the appropriate channel — don't loop on broken tooling.

---

## See also

- [FORGEKEY_DEVICE.md](../FORGEKEY_DEVICE.md) — device lifecycle, OTA
  apply flow, signature verification, rollback safety, NVS layout.
- [PEOPLE_COUNTER.md](../PEOPLE_COUNTER.md) — people-counter variant
  specifics.
- `scripts/build/gen-firmware-signing-key.sh` — keypair generator.
- `scripts/build/version.py` — PlatformIO build identity + artifact
  exporter.
- `src/ota/ota_updater.h` — OTA client API.
- `src/security/firmware_verify.h` — signature verification entry point.
