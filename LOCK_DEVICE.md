# ForgeKey Lock Device

Cabinet-lock build for ForgeKey devices. This variant controls a solenoid-driven
mortise lock with full supervision (reed switch + latch supervisor), IR beam
item detection, and physical key (mortise) override.

The active lock firmware is the ESP-IDF project under `esp32c6-lock/` for the
Seeed Studio XIAO ESP32-C6. The older Arduino prototype under `src/lock/` is
retained as a reference only and is not built by default.

## High-level flow

```
flash → boot → wifi → ntp → mqtt connected → SECURE state
                                      │
                    ┌─────────────────┼──────────────────┐
                    │                 │                  │
              Django publishes    Door opens →        Door opens
              signed unlock on    ACCESSING state     without valid
              forgekey/{mac}/     monitoring IR beam   command → ALARM
              command
                    │
              Device pulses solenoid
              → UNLOCKED state
                    │
              Door closes + latch engaged
              → back to SECURE
```

## Hardware Wiring

| Pin | Function | Type | Notes |
|-----|----------|------|-------|
| D0 | Latch Solenoid Power | Output | Drives MOSFET for 12V solenoid. Active HIGH. |
| D1 | Reed Switch | Input | Door frame sensor. INPUT_PULLUP. Closed = LOW. |
| D5 | Mortise Switch | Input | Physical key override. INPUT_PULLUP. Active LOW. |
| D6 | Latch Supervisor | Input | Internal lock bolt position. INPUT_PULLUP. Locked = LOW. |
| D4 | IR Receiver | Input | Breakbeam receiver. INPUT_PULLUP. Broken = LOW. |

### Solenoid driver

The solenoid is 12V. GPIO D0 drives a MOSFET that pulls the solenoid to
ground. The solenoid receives a short pulse (`FORGEKEY_LOCK_SOLENOID_PULSE_MS`,
default 1500ms) rather than being held energized continuously.

If you're using an actual strike that needs to be left open continuously, reach out to me and I'll add a feature flag for that build.

### Reed switch wiring

The reed switch is wired between D1 and GND with `INPUT_PULLUP`. When the
door is closed, the reed switch closes and D1 reads LOW. When the door is
open, D1 reads HIGH (pulled up internally).

### Latch supervisor wiring

The latch supervisor switch is wired between D6 and GND with `INPUT_PULLUP`.
When the latch bolt is fully thrown (locked), the switch closes and D6 reads
LOW. When the bolt is retracted (unlocked), D6 reads HIGH.

### IR breakbeam wiring

The IR receiver module is wired between D4 and GND with `INPUT_PULLUP`.
When the beam is intact (item present), the phototransistor conducts and D4
reads LOW. When the beam is broken (item removed), D4 reads HIGH.

> **Note:** Adjust `FORGEKEY_LOCK_IR_BEAM_BROKEN` macro if your IR module
> inverts the logic. Check the module's datasheet.

### Repeatable hardware artifacts

The source-controlled hardware pack for this build is
[`hardware/cabinet-lock/`](hardware/cabinet-lock/). It includes:

- [`pin-manifest.json`](hardware/cabinet-lock/pin-manifest.json) for the solenoid gate, reed switch, IR beam, mortise switch, latch supervisor, power rails, and protection components.
- [`wiring.svg`](hardware/cabinet-lock/wiring.svg) for rendered wiring review.
- [`bom.md`](hardware/cabinet-lock/bom.md) for MOSFET, flyback diode, resistors, sensors, solenoid, and supply ratings.
- [`harness-notes.md`](hardware/cabinet-lock/harness-notes.md) for connector pinouts and acceptance tests.
- [`wokwi/`](hardware/cabinet-lock/wokwi/) for a GPIO-level simulation, with limitations documented in [`simulation-limitations.md`](hardware/cabinet-lock/simulation-limitations.md).

### Mortise switch wiring

The physical key override switch is wired between D5 and GND with `INPUT_PULLUP`.
When the mortise key is turned, the switch closes and D5 reads LOW.

## State Machine

The lock operates in five states. All transitions are non-blocking (driven by
`esp_timer_get_time()` timing in `lock_state_tick()`).

### INITIALIZING

Blinking User LED. Connecting to Wi-Fi → Syncing NTP → Connecting to MQTT.

Transitions to **SECURE** once MQTT is connected and the lock capability is
ready.

### SECURE (Standard)

- **LED:** Red (steady)
- **Solenoid:** Off
- **Conditions:** Reed = Closed AND Latch = Locked

This is the normal resting state. The device publishes telemetry at
`FORGEKEY_LOCK_TELEMETRY_INTERVAL_MS` (default 10s).

Transitions to **UNLOCKED** when:
- A valid unlock command is received (solenoid pulses)
- Latch supervisor reads as unlocked (bolt retracted)

Transitions to **ALARM** when:
- Reed switch reads open (door opened) without a prior valid unlock

### UNLOCKED

- **LED:** Green (steady)
- **Solenoid:** Pulsed (short burst), then off
- **Conditions:** Latch disengaged

The device waits for the physical door to open. Telemetry continues to
publish with `last_trigger: "jwt"` or `"auto_unlock"`.

Transitions to **SECURE** when:
- Reed switch closes AND latch supervisor shows locked (door closed + bolt thrown)

Transitions to **ALARM** when:
- Mortise switch activates while in UNLOCKED state

### ACCESSING

- **LED:** Green (steady)
- **Conditions:** Reed = Open (door is open)

The door is open and the IR beam monitors for item removal. Telemetry
`item_present` reflects the IR beam state.

Transitions to **SECURE** when:
- Reed switch closes AND latch supervisor shows locked

Transitions to **ALARM** when:
- Mortise switch activates

### ALARM

- **LED:** Rapid Red/Green flash (`FORGEKEY_LOCK_ALARM_FLASH_MS`, default 200ms)
- **Conditions:** Door open without valid JWT or mortise key

Triggered when:
- Reed switch opens without a prior valid unlock command
- Mortise switch activates while unlocked

Transitions to **SECURE** when:
- Reed switch closes AND latch supervisor shows locked (door closed + bolt thrown)

## Supervision Logic

The hardware must satisfy a **Logical AND** to report a "Secure" status:

```
Status = SECURE  if  (Reed Switch == CLOSED) AND (Latch Supervisor == LOCKED)
```

Django should only trust `secure: true` when **both** conditions are met.
This prevents a false "secure" report from a forced-open door (reed switch
still closed but latch disengaged) or a partially latched door (latch
supervisor shows locked but door is ajar).

## Contract references

The lock protocol examples below are implementation notes for the ESP32-C6 lock
firmware. The canonical MQTT topic/payload matrix for the `cabinet_lock` device
class is [`docs/contracts/mqtt.md`](docs/contracts/mqtt.md), lock-related error
codes are defined in [`docs/contracts/error-codes.md`](docs/contracts/error-codes.md),
and shared payload schemas are registered in [`docs/schemas/`](docs/schemas/README.md).

## MQTT Protocol

### From Django to XIAO (Unlock Command)

**Topic:** `forgekey/{mac}/command`

Every operational command uses the signed command envelope below. Safety-sensitive
commands (`unlock`, `lockout`, `clear_lockout`, `init_ack`,
`emergency_unlock`, `commission`, and `restart`) require both a valid OMS
authenticator and an operator identity (`actor`; `operator_id` or
`operator.id` may also be included for OMS correlation).

**Common envelope:**
```json
{
  "cmd": "<verb>",
  "command_id": "cmd_01HV7J5H2G4R3M9K8N7P6Q5S4T",
  "issued_at": 1715150000,
  "expires_at": 1715150030,
  "nonce": "base64url-128-bit-random",
  "actor": "operator:ian",
  "jwt": "header.payload.signature"
}
```

Detached-signature envelopes are also accepted when `jwt` is omitted. The
signature is base64url raw ES256 `r||s` over this canonical string:
`cmd + "\n" + command_id + "\n" + issued_at + "\n" + expires_at + "\n" + nonce + "\n" + actor`.

```json
{
  "cmd": "lockout",
  "command_id": "cmd_01HV7J5H2G4R3M9K8N7P6Q5S4T",
  "issued_at": "1715150000",
  "expires_at": "1715150030",
  "nonce": "base64url-128-bit-random",
  "actor": "operator:ian",
  "signature": "base64url-es256-raw-rs"
}
```

The device validates:
1. JWT/detached-signature authenticator matches the active OMS command public key.
2. The UTC wall clock is valid before evaluating `issued_at`, `expires_at`, `timestamp`, or JWT `exp`.
3. `timestamp`/`issued_at` is within the configured skew window and any expiry has not passed.
4. `command_id` and `nonce` have not already been accepted by the replay cache.
5. Safety-sensitive commands include operator identity before local actuation.

If `clock_valid` is false, wall-clock signed commands are rejected with
`clock_invalid`. A server-provided challenge/nonce flow may bypass wall-clock
freshness only when the signed envelope includes `auth_flow: "challenge"` (or
`"nonce"`), `challenge`, or `server_nonce`; in that case the signed one-time
challenge and replay cache provide freshness.

### Operational command schemas

#### `lockout`
Enter sticky operator lockout; normal `unlock` commands are rejected and the
status LED fast-flashes until `clear_lockout` or `emergency_unlock`.

```json
{
  "cmd": "lockout",
  "command_id": "cmd_lockout_001",
  "issued_at": 1715150000,
  "expires_at": 1715150030,
  "nonce": "n-lockout-001",
  "actor": "operator:ian",
  "reason": "maintenance",
  "jwt": "header.payload.signature"
}
```

Ack: `{"cmd_ack":"lockout","command_id":"cmd_lockout_001","state":"LOCKOUT",...}`

#### `clear_lockout`
Clear sticky operator lockout and return to `SECURE` when reed and latch both
report secure, otherwise to `ALARM`.

```json
{
  "cmd": "clear_lockout",
  "command_id": "cmd_clear_lockout_001",
  "issued_at": 1715150100,
  "expires_at": 1715150130,
  "nonce": "n-clear-lockout-001",
  "actor": "operator:ian",
  "jwt": "header.payload.signature"
}
```

#### `init_ack`
OMS acknowledges provisioning/boot handoff. The device leaves `INITIALIZING` or
`COMMISSIONING` and evaluates physical security (`SECURE` vs `ALARM`).

```json
{
  "cmd": "init_ack",
  "command_id": "cmd_init_ack_001",
  "issued_at": 1715150200,
  "expires_at": 1715150230,
  "nonce": "n-init-ack-001",
  "actor": "operator:ian",
  "jwt": "header.payload.signature"
}
```

#### `emergency_unlock`
Pulses the solenoid immediately, including while `LOCKOUT` is active. This is
audited with an `emergency_unlock` event carrying the command ID.

```json
{
  "cmd": "emergency_unlock",
  "command_id": "cmd_emergency_001",
  "issued_at": 1715150300,
  "expires_at": 1715150330,
  "nonce": "n-emergency-001",
  "actor": "operator:ian",
  "incident_id": "inc_123",
  "jwt": "header.payload.signature"
}
```

#### `commission`
Enter commissioning mode for installation or service. The status LED slow-flashes
and telemetry reports `commissioning_active: true` until `init_ack`.

```json
{
  "cmd": "commission",
  "command_id": "cmd_commission_001",
  "issued_at": 1715150400,
  "expires_at": 1715150430,
  "nonce": "n-commission-001",
  "actor": "operator:ian",
  "asset_id": "fk_asset_123",
  "jwt": "header.payload.signature"
}
```

### From XIAO to Django (Telemetry)

**Topic:** `forgekey/{mac}/cabinet_lock/status`

**Payload:**
```json
{
  "mac": "aabbccddeeff",
  "secure": true,
  "item_present": true,
  "uptime": 3600,
  "clock_valid": true,
  "ntp_synced": true,
  "epoch_time": 1715150000,
  "last_ntp_sync_age_s": 8,
  "uptime_ms": 3600000,
  "firmware_version": "0.1.0",
  "build_target": "esp32c6-lock",
  "framework": "esp-idf",
  "last_trigger": "jwt",
  "state": "SECURE",
  "reed_closed": true,
  "latch_locked": true,
  "ir_broken": false,
  "mortise_active": false,
  "lockout_active": false,
  "commissioning_active": false,
  "tamper_active": false
}
```

**`last_trigger` values:**
| Value | Meaning |
|-------|---------|
| `"signed_command"` | Unlock via valid signed MQTT command |
| `"mortise"` | Physical key (mortise switch) triggered |
| `"auto_unlock"` | Auto-unlock (if `AUTO_RELOCK_MS` > 0) |
| `"door_close"` | Door closed and latch engaged |
| `"alarm_timeout"` | Alarm condition cleared |
| `"lockout"` | Operator lockout entered |
| `"clear_lockout"` | Operator lockout cleared |
| `"commission"` | Commissioning mode entered |
| `"init_ack"` | OMS/operator initialization acknowledgement |
| `"emergency_unlock"` | Emergency unlock pulse |
| `"unknown"` | Default / initial |


### Event messages

Sensor and safety events are published on `forgekey/{mac}/status` as soon as the
debounced value changes. Every event includes `clock_valid`/`epoch_time` from
`forgekey_time_add_json(...)`, monotonic `uptime`, current sensor values, and the
most recent accepted `command_id` (or the initiating command ID for command-
driven mode events).

```json
{
  "event": "reed",
  "active": false,
  "command_id": "cmd_unlock_001",
  "state": "UNLOCKED",
  "uptime": 3601000,
  "reed_closed": false,
  "latch_locked": false,
  "ir_broken": false,
  "mortise_active": false,
  "tamper_active": false,
  "lockout_active": false,
  "commissioning_active": false,
  "clock_valid": true,
  "epoch_time": 1715150001
}
```

Event names: `tamper`, `mortise_key`, `latch`, `reed`, `ir_beam`, `lockout`,
`commissioning`, and `emergency_unlock`.

## Time requirements and clock health

The ESP32-C6 lock has no trusted RTC. After WiFi connects it starts SNTP in UTC
through `esp32c6-lock/main/forgekey_time.{h,c}` using `pool.ntp.org` and
`time.nist.gov`. The module tracks:

- `clock_valid`: epoch is plausible and the last SNTP sync age is within the
  configured max age (default 24h).
- `ntp_synced`: SNTP has set the wall clock at least once this boot.
- `epoch_time`: timezone-neutral Unix epoch seconds.
- `last_ntp_sync_age_s`: monotonic age of the last sync.
- `uptime_ms`: monotonic uptime for state-machine timing.

MQTT telemetry, status snapshots, health/OTA status, command acknowledgements,
and `/api/status` include these fields. The lock state machine continues to use
monotonic time for pulses, debounce, and alarms; wall-clock time is only used
for TLS and signed-command freshness/expiry.

## Embedded Web Page

The lock serves a status page at `http://<device-ip>/` when connected to WiFi.

### Pages

| Path | Content |
|------|---------|
| `/` | HTML status page with live sensor readings and OpenMakerSuite link |
| `/api/status` | JSON telemetry (same as MQTT publish) |

### Status Page Features

- Current lock state (SECURE / UNLOCKED / ACCESSING / ALARM)
- Color-coded state indicator with CSS animations for ALARM
- Sensor readings: reed switch, latch supervisor, IR beam, mortise switch
- Device uptime display
- Uppercase link to `https://openmakersuite.com` for generating unlock codes

### No Code Prompt

If a user visits the device IP without an unlock code, the status page
displays a prominent call-to-action directing them to the OpenMakerSuite
dashboard to generate an unlock request or code.

## Compile-Time Configuration

Primary defaults live in `esp32c6-lock/main/lock_config.h`. The ESP-IDF lock
project also exposes matching Kconfig entries in `esp32c6-lock/main/Kconfig.projbuild`
for native `idf.py` workflows.

| Macro | Default | Description |
|-------|---------|-------------|
| `FORGEKEY_LOCK_SOLENOID_PIN` | `0` | Solenoid MOSFET control pin |
| `FORGEKEY_LOCK_REED_PIN` | `1` | Reed switch input pin |
| `FORGEKEY_LOCK_MORTISE_PIN` | `5` | Mortise (physical key) switch pin |
| `FORGEKEY_LOCK_LATCH_SUPERVISOR_PIN` | `6` | Latch bolt position sensor pin |
| `FORGEKEY_LOCK_IR_BEAM_PIN` | `4` | IR breakbeam receiver pin |
| `FORGEKEY_LOCK_SOLENOID_ACTIVE` | `HIGH` | Solenoid active level |
| `FORGEKEY_LOCK_SOLENOID_PULSE_MS` | `1500` | Solenoid pulse duration |
| `FORGEKEY_LOCK_AUTO_RELOCK_MS` | `0` | Auto-relock delay (0 = disabled) |
| `FORGEKEY_LOCK_TELEMETRY_INTERVAL_MS` | `10000` | Telemetry publish interval |
| `FORGEKEY_LOCK_CMD_TIMESTAMP_TOLERANCE_S` | `60` | Signed-command timestamp tolerance |
| `FORGEKEY_LOCK_JWT_EXPIRY_S` | `30` | JWT max expiry window |
| `FORGEKEY_LOCK_STATUS_LED_PIN` | `-1` | Optional status LED pin (-1 disables) |
| `FORGEKEY_LOCK_LOCKOUT_FLASH_MS` | `100` | Lockout LED flash rate |
| `FORGEKEY_LOCK_COMMISSION_FLASH_MS` | `700` | Commissioning LED flash rate |
| `FORGEKEY_LOCK_ALARM_FLASH_MS` | `200` | Alarm LED flash rate |
| `FORGEKEY_LOCK_REED_DEBOUNCE_MS` | `200` | Reed switch debounce time |
| `FORGEKEY_LOCK_LATCH_DEBOUNCE_MS` | `200` | Latch supervisor debounce time |
| `FORGEKEY_LOCK_IR_DEBOUNCE_MS` | `500` | IR beam debounce time |
| `FORGEKEY_LOCK_MORTISE_DEBOUNCE_MS` | `200` | Mortise switch debounce time |

## Building

```bash
# Build the active ESP32-C6 lock variant
cd esp32c6-lock
. $IDF_PATH/export.sh
idf.py build
```

```bash
# From the repo root, build the Arduino variants
pio run
```

## Security Notes

1. **OMS command public key:** Provision the OMS ES256 command public key before
   production deployment and rotate it through the provisioning/configuration
   channel when operators change.

2. **Signed command verification:** The ESP32-C6 lock firmware validates signed
   command envelopes, rejects replayed `command_id`/`nonce` values, and refuses
   wall-clock freshness checks while `clock_valid` is false unless the command
   uses a signed server challenge/nonce flow.

3. **Solenoid Safety:** The solenoid pulse is short (default 1.5s) to prevent
   coil overheating. The latch supervisor feedback ensures the bolt actually
   moved before transitioning states.

4. **Supervision AND:** The `secure` status requires both reed closed AND
   latch locked. Django should treat `secure: false` as a potential intrusion
   and trigger appropriate alerts.

5. **Security Header Sync:** If you rotate the OTA signing key or OMS CA in
   `src/security/`, run `python3 esp32c6-lock/scripts/esp32c6-sync-security.py`
   before building the lock firmware so `esp32c6-lock/main/` stays in sync.

## Telemetry Change Detection

Telemetry is published on state changes and at the configured interval. The
device tracks the last known sensor values and only publishes when something
changes (state transition or interval timer).

## Out of Scope

- OTA firmware updates (supported via shared `ota_updater` infrastructure)
- First-boot provisioning (supported via shared `provisioning` infrastructure)
- Captive portal WiFi setup (supported via shared `wifi_setup` infrastructure)
- Credential rotation (supported via shared `credential_rotation` infrastructure)
- BLE capabilities (disabled in lock build to save RAM/flash)
- Camera / photo upload (not applicable to lock build)
