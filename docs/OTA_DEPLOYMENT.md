# OTA Deployment Workflow

End-to-end walkthrough for shipping a new firmware image to ForgeKey
devices via OpenMakerSpace (OMS). This is the **operator** view — what to
type, in what order, and where the bytes go. For the device-side OTA
internals (apply flow, rollback, status payloads, signature verification),
see [FORGEKEY_DEVICE.md "OTA firmware updates"](../FORGEKEY_DEVICE.md#ota-firmware-updates).

The four sections below are cumulative: do (1) once per fleet, then (2)–(4)
each release.

- [1. One-time signing-key setup](#1-one-time-signing-key-setup)
- [2. Build a deployable binary](#2-build-a-deployable-binary)
- [3. Upload and dispatch via OMS](#3-upload-and-dispatch-via-oms)
- [4. Troubleshooting](#4-troubleshooting)

---

## 1. One-time signing-key setup

ForgeKey OTA images are signed with **ECDSA P-256**. The device verifies
each dispatch against a public key compiled into firmware
(`src/security/firmware_pubkey.h`). The matching private key lives only on
the OMS server, exported as the `FORGEKEY_FIRMWARE_SIGNING_KEY` env var.

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

ForgeKey ships two device variants from one tree, each as its own
PlatformIO env:

| PlatformIO env | Variant slug | Purpose |
|---|---|---|
| `seeed_xiao_esp32s3` | `people-counter` | Camera-based occupancy counting |
| `seeed_xiao_esp32s3_temperature` | `temperature-sensor` | DHT 21 temperature/humidity |

The two binaries are **not interchangeable**. A temperature-sensor device
cannot run a people-counter image and vice versa — dispatch the right one.

### 2.1 Bump the version

```bash
# Edit src/provisioning/device_config.h, bump FORGEKEY_FIRMWARE_VERSION.
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

# Or build both in one go:
~/.platformio/penv/bin/platformio run -e seeed_xiao_esp32s3 -e seeed_xiao_esp32s3_temperature
```

### 2.3 Locate the binary

PlatformIO writes the raw build output to:

```
.pio/build/<env>/firmware.bin
```

But you almost never need that path directly. The post-build hook
(`scripts/build/version.py`) automatically copies the merged binary to a
versioned artifact path keyed on the variant, version, and short git SHA:

```
artifacts/
  forgekey-people-counter-<version>-<commit>.bin
  forgekey-people-counter-<version>-<commit>.bin.sha256
  forgekey-people-counter-latest.bin
  forgekey-people-counter-latest.bin.sha256

  forgekey-temperature-sensor-<version>-<commit>.bin
  forgekey-temperature-sensor-<version>-<commit>.bin.sha256
  forgekey-temperature-sensor-latest.bin
  forgekey-temperature-sensor-latest.bin.sha256
```

The `.sha256` file holds the lowercase hex digest with no filename suffix
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
   `forgekey-people-counter-0.2.0-abcd123.bin`).
3. Set the **version** field to match the embedded build version
   (semver, e.g. `0.2.0`). OMS uses this to populate the dispatch
   payload's `version` field, which the device echoes back in registration
   pings.
4. Set the **hardware revision tag** if your fleet has multiple revs
   (e.g. `esp32-s3-sense-v1`). OMS uses this when fleet-targeting to
   avoid shipping the wrong binary to incompatible hardware.
5. **Save.** OMS computes the SHA-256, base64-encodes the ECDSA(P-256)
   signature using the private key from
   `FORGEKEY_FIRMWARE_SIGNING_KEY`, and stores both alongside the binary.

### 3.2 Dispatch to a device or fleet

From the firmware-update detail page:

- **Deploy to device**: pick a single device. OMS publishes one dispatch
  message and watches the device's status topic for progress.
- **Deploy to fleet**: targets every device matching the variant +
  hardware revision. OMS fans the dispatch out via Celery — one publish
  per device.

The backend Celery task publishes the dispatch JSON to:

```
forgekey/<mac>/<kind>/firmware
```

where `<kind>` is `people_counter` or `temperature_sensor` depending on
the device's registered sensor kind. The exact topic was returned to the
device at registration in the `mqtt_topic_for_firmware` field of the
register response, persisted to NVS as `fw_topic`, and is the topic the
device subscribes to on every boot.

The dispatch payload shape:

```json
{
  "url": "https://oms.openmakerspace.org/static/firmware/<filename>.bin",
  "sha256": "f3b5...64hex",
  "signature": "MEUCIQ...base64-DER-ECDSA(P-256)",
  "version": "0.2.0",
  "mandatory": false
}
```

`mandatory=true` makes the device apply immediately even if a photo
upload is in flight. `mandatory=false` defers the apply if the device
uploaded a photo in the last 5 seconds.

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

## 4. Troubleshooting

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
