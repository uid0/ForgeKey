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
              unlock cmd on       ACCESSING state     without valid
              cabinets/{mac}/cmd  monitoring IR beam   JWT → ALARM state
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

## MQTT Protocol

### From Django to XIAO (Unlock Command)

**Topic:** `cabinets/{mac}/cmd`

**Payload:**
```json
{
  "token": "header.payload.signature",
  "timestamp": 1715150000
}
```

- `token`: HS256 JWT (30s expiry)
- `timestamp`: Server epoch seconds at JWT issue time

The device validates:
1. JWT signature matches `FORGEKEY_LOCK_JWT_SECRET`
2. `timestamp` is within `FORGEKEY_LOCK_CMD_TIMESTAMP_TOLERANCE_S` (60s) of device clock
3. JWT `exp` field has not passed

### From XIAO to Django (Telemetry)

**Topic:** `forgekey/{mac}/cabinet_lock/status`

**Payload:**
```json
{
  "mac": "aabbccddeeff",
  "secure": true,
  "item_present": true,
  "uptime": 3600,
  "firmware_version": "0.1.0",
  "build_target": "esp32c6-lock",
  "framework": "esp-idf",
  "last_trigger": "jwt",
  "state": "SECURE",
  "reed_closed": true,
  "latch_locked": true,
  "ir_broken": false,
  "mortise_active": false
}
```

**`last_trigger` values:**
| Value | Meaning |
|-------|---------|
| `"jwt"` | Unlock via valid MQTT command |
| `"mortise"` | Physical key (mortise switch) triggered |
| `"auto_unlock"` | Auto-unlock (if `AUTO_RELOCK_MS` > 0) |
| `"door_close"` | Door closed and latch engaged |
| `"alarm_timeout"` | Alarm condition cleared |
| `"unknown"` | Default / initial |

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
| `FORGEKEY_LOCK_CMD_TIMESTAMP_TOLERANCE_S` | `60` | JWT timestamp tolerance |
| `FORGEKEY_LOCK_JWT_EXPIRY_S` | `30` | JWT max expiry window |
| `FORGEKEY_LOCK_JWT_SECRET` | `...` | HMAC-SHA256 shared secret (change in production!) |
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

1. **JWT Secret:** `FORGEKEY_LOCK_JWT_SECRET` must be changed before any
   production deployment. The default value is intentionally unsafe.

2. **HMAC Verification:** The ESP32-C6 lock firmware verifies HS256 JWT
   signatures with mbedTLS HMAC-SHA256 in `esp32c6-lock/main/lock_state.c`.
   Valid tokens still depend on the device clock being NTP-synced because the
   firmware enforces both timestamp tolerance and token expiry.

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
