# ForgeKey Device Commands

Per-device control plane between OMS and ForgeKey firmware. OMS publishes
JSON commands on `forgekey/<mac>/command`; the device replies with structured
acks on `forgekey/<mac>/status`.

`<mac>` is the bare lowercase 12-hex MAC (no separators), e.g.
`aabbcc112233`.

## Topic shape

| Direction | Topic | Producer | Consumer |
|-----------|-------|----------|----------|
| Command   | `forgekey/<mac>/command` | OMS | Device |
| Ack/state | `forgekey/<mac>/status`  | Device | OMS |

Both use QoS 0, retain=false. The MQTT username is `forgemqtt`; the password
is the per-device JWT issued at registration.

## Common ack shape

Every command replies on the status topic with:

```json
{ "cmd_ack": "<original cmd>", ...command-specific fields... }
```

A second ack may follow for asynchronous commands (e.g. `capture` reports
both `queued:true` and a later `upload_status`).

Unknown commands receive:

```json
{ "cmd_ack": "<original cmd>", "error": "unknown_command" }
```

Silent drops are explicitly avoided — an OMS UI seeing no ack indicates a
broker/network problem, not a malformed command.

## Commands

### `restart`

```json
{ "cmd": "restart" }
```

Ack:

```json
{ "cmd_ack": "restart", "in_ms": 1000 }
```

The device flushes the ack to the broker and reboots ~1s later via
`ESP.restart()`. No further ack — OMS infers reboot from the device
re-publishing its capabilities announcement on next boot.

### `capture` (alias: `capture_photo`)

```json
{ "cmd": "capture" }
```

People-counter builds queue a one-shot photo capture + upload at the next
loop iteration (gated by the same post-inference quiet window as the
periodic uploader, so TLS doesn't tear down mid-handshake).

Initial ack:

```json
{ "cmd_ack": "capture", "queued": true }
```

Final ack (after upload completes or fails):

```json
{ "cmd_ack": "capture", "upload_status": "ok" }
{ "cmd_ack": "capture", "upload_status": "failed", "reason": "transport_error" }
```

`reason` enumeration:
- `auth_expired` — JWT rejected (401); device clears creds for re-register
- `server_error` — OMS returned 5xx
- `transport_error` — TLS / socket failure
- `bad_response` — non-2xx, non-401, non-5xx
- `capture_failed` — camera returned no frame

Builds without a camera (e.g. `seeed_xiao_esp32s3_temperature`) reject the
command up front:

```json
{ "cmd_ack": "capture", "queued": false, "error": "capability_unsupported" }
```

People-counter builds where the camera failed to init at boot:

```json
{ "cmd_ack": "capture", "queued": false, "error": "camera_unavailable" }
```

### `status` / `ping`

```json
{ "cmd": "status" }
{ "cmd": "ping" }
```

Both publish a snapshot on the status topic without changing device state.
Useful for "is the device alive" polling without relying on its periodic
publishes.

```json
{
  "cmd_ack": "status",
  "capabilities": ["people_counter", "status_led"],
  "firmware_version": "0.1.0",
  "free_heap": 187344,
  "rssi": -57,
  "uptime_ms": 421337,
  "mac": "aabbcc112233"
}
```

### `identify`

Operator wants to physically locate the device. Same as `blink:start` but
auto-clears after a configurable duration.

```json
{ "cmd": "identify" }
{ "cmd": "identify", "duration_s": 60 }
```

Default `duration_s` is 30 (override via `-DIDENTIFY_DEFAULT_DURATION_S=N`
at build time).

Acks:

```json
{ "blink": "on" }
{ "cmd_ack": "identify", "duration_s": 30 }
```

When the timer expires, the device echoes:

```json
{ "blink": "off" }
```

### `blink`

Operator-toggled identify LED with no auto-clear.

```json
{ "cmd": "blink" }                       // toggle
{ "cmd": "blink", "on": true }
{ "cmd": "blink", "on": false }
{ "cmd": "blink", "action": "start" }
{ "cmd": "blink", "action": "stop" }
{ "cmd": "blink", "action": "toggle" }
```

Echo on transition:

```json
{ "blink": "on" }
{ "blink": "off" }
```

No echo on no-op (already in requested state).

## Out of scope

- OTA dispatch — separate topic (`forgekey/<mac>/<kind>/firmware`), see
  `docs/ota/` for the dispatch payload schema.
- Capability-specific commands (e.g. people-counter zone reconfiguration) —
  reserved for future per-capability subtopics like
  `forgekey/<mac>/people_counter/command`.

---

# Lock Device Commands

The cabinet-lock build (`seeed_xiao_esp32s3_lock`) receives unlock commands
on a separate topic (`cabinets/<mac>/cmd`) rather than the standard command
topic. This keeps unlock signaling separate from operator commands (blink,
identify, restart, etc.).

## Topic shape

| Direction | Topic | Producer | Consumer |
|-----------|-------|----------|----------|
| Unlock cmd | `cabinets/<mac>/cmd` | Django / OMS | Device |
| Telemetry | `forgekey/<mac>/cabinet_lock/status` | Device | OMS |
| Command ack | `forgekey/<mac>/status` | Device | OMS |

## Unlock command

Django publishes a JWT-wrapped unlock command:

```json
{ "token": "<HS256 JWT>", "timestamp": 1715150000 }
```

- `token`: HS256 JWT issued by Django. Contains `exp` (30s expiry) and
  `timestamp` (epoch seconds).
- `timestamp`: server clock at JWT issue time. Device rejects if the
  difference from its own NTP-synced clock exceeds `FORGEKEY_LOCK_CMD_TIMESTAMP_TOLERANCE_S` (default 60s).

Device ack (on success):

```json
{ "cmd_ack": "unlock", "state": "unlocked" }
```

Device ack (on failure):

```json
{ "cmd_ack": "unlock", "error": "invalid_token" }
```

## Telemetry

The lock publishes periodic telemetry to `forgekey/<mac>/cabinet_lock/status`
at `FORGEKEY_LOCK_TELEMETRY_INTERVAL_MS` (default 10s):

```json
{
  "mac": "aabbcc112233",
  "secure": true,
  "item_present": true,
  "uptime": 421337,
  "last_trigger": "jwt",
  "state": "SECURE",
  "reed_closed": true,
  "latch_locked": true,
  "ir_broken": false,
  "mortise_active": false
}
```

Fields:

| Field | Type | Description |
|-------|------|-------------|
| `mac` | string | Bare lowercase 12-hex device MAC |
| `secure` | bool | `reed_closed AND latch_locked` (supervision AND) |
| `item_present` | bool | `!ir_broken` (IR beam intact = item present) |
| `uptime` | number | `millis()` uptime in ms |
| `last_trigger` | string | Last unlock trigger: `"jwt"`, `"mortise"`, `"door_close"`, `"alarm_timeout"` |
| `state` | string | Current state: `INITIALIZING`, `SECURE`, `UNLOCKED`, `ACCESSING`, `ALARM` |
| `reed_closed` | bool | Raw reed switch state |
| `latch_locked` | bool | Raw latch supervisor state |
| `ir_broken` | bool | Raw IR beam state |
| `mortise_active` | bool | Raw mortise (physical key) switch state |

## Lock States

| State | LED | Description |
|-------|-----|-------------|
| `INITIALIZING` | Blinking | WiFi → NTP → MQTT connecting |
| `SECURE` | Red (steady) | Reed closed AND latch locked. Cabinet is secure. |
| `UNLOCKED` | Green (steady) | Latch disengaged. Waiting for door to open. |
| `ACCESSING` | Green (steady) | Door open. IR beam monitoring item removal. |
| `ALARM` | Rapid Red/Green flash | Door open without valid JWT or physical key |

### Supervision Logic

`SECURE` is reported only when **both** conditions are true:
- Reed switch == CLOSED (door is shut)
- Latch supervisor == LOCKED (bolt is thrown)

If either condition is false, the device reports `secure: false`.

## Embedded Web Page

The lock serves a status page at `http://<device-ip>/` showing:
- Current lock state
- Real-time sensor readings (reed, latch, IR, mortise)
- Uptime
- Link to OpenMakerSuite for generating unlock codes

JSON API at `http://<device-ip>/api/status` returns the same telemetry as
the MQTT publish.

---

# BLE Capabilities

The firmware includes four BLE capabilities that can be toggled at runtime
via the `forgekey/<mac>/config` topic or disabled at compile time.

## Compile-time toggles

All four BLE capabilities are enabled by default. Each can be disabled by
uncommenting the corresponding `#define` in `src/provisioning/device_config.h`:

```cpp
#define FORGEKEY_DISABLE_BLE_SCANNER
#define FORGEKEY_DISABLE_BLE_BEACON
#define FORGEKEY_DISABLE_BLE_RELAY
#define FORGEKEY_DISABLE_BLE_EQUIPMENT
```

Or via PlatformIO build flags:

```ini
build_flags =
    -DFORGEKEY_DISABLE_BLE_SCANNER
    -DFORGEKEY_DISABLE_BLE_BEACON
```

## Runtime BLE enable/disable

Control BLE via the config topic (`forgekey/<mac>/config`):

```json
// Enable all BLE capabilities
{ "cmd": "set_ble", "enabled": true }

// Disable all BLE capabilities
{ "cmd": "set_ble", "enabled": false }

// Enable only the scanner
{ "cmd": "set_ble", "scanner": true, "beacon": false, "relay": false, "equipment": false }

// Enable only the beacon
{ "cmd": "set_ble", "scanner": false, "beacon": true, "relay": false, "equipment": false }
```

Ack:

```json
{ "cmd_ack": "set_ble", "enabled": true, "scanner": true, "beacon": true, "relay": false, "equipment": false }
```

This is the recommended way to completely disable BLE on a device — it
stops all BLE scanning and advertising without requiring a reboot.

## BLE MQTT Topics

| Topic | Direction | Producer | Consumer |
|-------|-----------|----------|----------|
| `forgekey/<mac>/ble/devices` | Device → OMS | `ble_scanner` | OMS |
| `forgekey/<mac>/ble/beacons` | Device → OMS | `ble_beacon` | OMS |
| `forgekey/<mac>/ble/peers` | Device → OMS | `ble_relay` | OMS |
| `forgekey/<mac>/ble/equipment` | Device → OMS | `ble_equipment` | OMS |

## Capability: BLE Scanner (`ble_scanner`)

Periodically scans for BLE advertisements and publishes unique devices with
RSSI values to `forgekey/<mac>/ble/devices`.

**Scan cadence:** 60s interval, 5s scan duration (configurable via
`BLE_SCAN_INTERVAL_MS` and `BLE_SCAN_DURATION_S` at build time).

**Payload:**

```json
{
  "devices": [
    {
      "mac": "aabbcc112233",
      "rssi": -55,
      "type": "general"
    },
    {
      "mac": "ddeeff445566",
      "rssi": -72,
      "type": "ibeacon",
      "uuid": "E2C56DB5-DFFB-48D2-B060-D0F5A7100000",
      "major": 1,
      "minor": 2
    },
    {
      "mac": "112233445566",
      "rssi": -60,
      "type": "ibeacon",
      "uuid": "A1B2C3D4-E5F6-7890-ABCD-EF1234567890",
      "peer": true
    }
  ],
  "count": 3,
  "timestamp": 1716000000
}
```

`type` values:
- `"general"` — standard BLE advertisement
- `"ibeacon"` — Apple iBeacon (parsed from manufacturer-specific data)
- `"peer"` — ForgeKey device (detected via beacon UUID `A1B2C3D4-E5F6-7890-ABCD-EF1234567890`)

**Filtering:**
- Own MAC excluded
- RSSI threshold: only devices > -90 dBm (configurable via `BLE_SCAN_RSSI_THRESHOLD`)
- Deduplication: devices seen in last 60 scan cycles are excluded (configurable via `BLE_DEDUP_WINDOW`)
- Max devices per scan: 50 (configurable via `BLE_MAX_DEVICES`)

**Commands:**

```json
// Trigger an immediate scan (regardless of cadence)
{ "cmd": "ble_scan" }

// Stop an ongoing scan
{ "cmd": "ble_scan_stop" }
```

Acks:

```json
{ "cmd_ack": "ble_scan", "queued": true }
{ "cmd_ack": "ble_scan_stop" }
```

## Capability: BLE Beacon (`ble_beacon`)

Broadcasts a ForgeKey iBeacon so other devices and phones can discover us.

**Beacon parameters:**
- UUID: `A1B2C3D4-E5F6-7890-ABCD-EF1234567890`
- Major: first 2 hex chars of device MAC (device group)
- Minor: last 4 hex chars of device MAC (individual device)
- TX power: -6 dBm (calibrated)
- Duty cycle: 100ms on, 2460ms off (~10%)
- Configurable via `BLE_BEACON_INTERVAL_MS` and `BLE_BEACON_ADV_ON_MS`

**Commands:**

```json
// Start continuous beacon advertising (e.g. during "identify")
{ "cmd": "beacon", "action": "start" }

// Stop continuous advertising, resume duty-cycled mode
{ "cmd": "beacon", "action": "stop" }
```

Acks:

```json
{ "cmd_ack": "beacon", "action": "start" }
{ "cmd_ack": "beacon", "action": "stop" }
```

## Capability: Inter-Device Relay (`ble_relay`)

When MQTT is down, stores messages in an in-memory ring buffer and forwards
them to nearby ForgeKey devices via BLE GATT.

**Queue parameters:**
- Max messages: 20 (configurable via `BLE_RELAY_QUEUE_SIZE`)
- Message TTL: 1 hour (configurable via `BLE_RELAY_MSG_TTL_MS`)
- Peer timeout: 5 minutes (configurable via `BLE_RELAY_PEER_TIMEOUT_MS`)

**Message format (over BLE GATT):**

```json
{
  "src": "aabbcc112233",
  "dst": "ddeeff445566",
  "topic": "forgekey/ddeeff445566/ble/devices",
  "payload": {"devices":[...], "count": 5, "timestamp": 1716000000},
  "ts": 1716000000,
  "relay": true
}
```

**GATT service UUID:** `B1B2C3D4-E5F6-7890-ABCD-EF1234567891`

**Sync on MQTT reconnect:** When the device reconnects to the broker, all
undelivered messages in the queue are published to their original topics
with a `"relay": true` flag so OMS knows they were relayed.

## Capability: Equipment Tracking (`ble_equipment`)

Detects ESP32-based BLE beacon tags (equipment) and reports their proximity
zones to OMS.

**Configuration** (via `forgekey/<mac>/config`):

```json
{
  "cmd": "set_equipment",
  "tags": [
    {
      "name": "Oscilloscope",
      "mac": "aabbcc112233",
      "type": "mac",
      "proximity_zone": "near"
    },
    {
      "name": "3D Printer",
      "ibeacon_uuid": "11111111-2222-3333-4444-555555555555",
      "type": "ibeacon",
      "proximity_zone": "mid"
    }
  ]
}
```

Tag types:
- `"mac"` — match by bare MAC address
- `"ibeacon"` — match by iBeacon UUID

Proximity zones (based on RSSI):
- `"near"` — RSSI > -60 dBm
- `"mid"` — RSSI -60 to -75 dBm
- `"far"` — RSSI < -75 dBm

**Hysteresis:**
- Detect: 3 consecutive scans showing the tag
- Lost: 6 consecutive scans without the tag

**Events published to `forgekey/<mac>/ble/equipment`:**

Detected:
```json
{
  "cmd_ack": "equipment",
  "event": "detected",
  "name": "Oscilloscope",
  "mac": "aabbcc112233",
  "rssi": -55,
  "zone": "near"
}
```

Lost:
```json
{
  "cmd_ack": "equipment",
  "event": "lost",
  "name": "Oscilloscope",
  "mac": "aabbcc112233",
  "rssi": -55,
  "zone": "near"
}
```

**Commands:**

```json
// List configured equipment tags
{ "cmd": "equipment_list" }

// Configure tags (note: actual config goes via forgekey/<mac>/config)
{ "cmd": "equipment_set", "tags": [...] }
```

Acks:

```json
{
  "cmd_ack": "equipment_list",
  "count": 2,
  "tags": [
    { "name": "Oscilloscope", "mac": "aabbcc112233", "type": "mac", "zone": "near" },
    { "name": "3D Printer", "ibeacon_uuid": "11111111-...", "type": "ibeacon", "zone": "mid" }
  ]
}
{ "cmd_ack": "equipment_set", "note": "use_config_topic" }
```

## NVS Storage

| Namespace | Key | Type | Purpose |
|-----------|-----|------|---------|
| `forgekey_ble` | `peers` | str | JSON peer table (MAC + RSSI + timestamp) |
| `forgekey_ble` | `tags` | str | JSON equipment tag list |
| `forgekey_relay` | `queue` | str | JSON relay message ring buffer |

## Config Topic Commands

The `forgekey/<mac>/config` topic handles both credential rotation and
capability configuration.

### `set_ble` — Enable/disable BLE capabilities

```json
{ "cmd": "set_ble", "enabled": true }
{ "cmd": "set_ble", "enabled": false }
{ "cmd": "set_ble", "scanner": true, "beacon": false, "relay": false, "equipment": false }
```

Ack:

```json
{ "cmd_ack": "set_ble", "enabled": true, "scanner": true, "beacon": true, "relay": false, "equipment": false }
```

### `set_equipment` — Configure equipment tags

```json
{
  "cmd": "set_equipment",
  "tags": [
    { "name": "Oscilloscope", "mac": "aabbcc112233", "type": "mac", "proximity_zone": "near" }
  ]
}
```

Ack:

```json
{ "cmd_ack": "set_equipment", "ok": true }
```

Tags are persisted to NVS (`forgekey_ble/tags`) and survive reboots.

### `forget_ble` — Clear all BLE data

```json
{ "cmd": "forget_ble" }
```

Clears:
- All BLE peer entries
- All equipment tags
- All relay queue messages

Ack:

```json
{ "cmd_ack": "forget_ble" }
```

## Configuration Summary

| Setting | Default | Override |
|---------|---------|----------|
| BLE enabled at boot | `1` (enabled) | `#define FORGEKEY_BLE_ENABLED_DEFAULT 0` in `device_config.h` |
| Scan interval | `60000` ms | `-DBLE_SCAN_INTERVAL_MS=NNN` |
| Scan duration | `5` s | `-DBLE_SCAN_DURATION_S=N` |
| Scan RSSI threshold | `-90` dBm | `-DBLE_SCAN_RSSI_THRESHOLD=-NN` |
| Max devices per scan | `50` | `-DBLE_MAX_DEVICES=N` |
| Dedup window | `60` scans | `-DBLE_DEDUP_WINDOW=N` |
| Beacon interval | `2560` ms | `-DBLE_BEACON_INTERVAL_MS=NNNN` |
| Beacon adv on time | `100` ms | `-DBLE_BEACON_ADV_ON_MS=NNN` |
| Relay queue size | `20` messages | `-DBLE_RELAY_QUEUE_SIZE=N` |
| Relay peer timeout | `300000` ms | `-DBLE_RELAY_PEER_TIMEOUT_MS=NNNNN` |
| Relay message TTL | `3600000` ms | `-DBLE_RELAY_MSG_TTL_MS=NNNNNN` |
