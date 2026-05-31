# ESP32-C6 Lock Device

ESP-IDF native build for the ForgeKey cabinet-lock on Seeed Studio XIAO ESP32-C6.
This is the active lock firmware target in the repo.

## Build Instructions

```bash
# Install ESP-IDF (v5.0+) with ESP32-C6 support
# https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/get-started/index.html

# Set up ESP-IDF environment
. $IDF_PATH/export.sh

# Build from esp32c6-lock/
idf.py build

# Flash
idf.py -p /dev/ttyUSB0 flash

# Monitor
idf.py monitor
```

## Hardware

| Pin | Function | Type |
|-----|----------|------|
| D0 | Solenoid MOSFET | Output |
| D1 | Reed Switch | Input (INPUT_PULLUP) |
| D5 | Mortise Switch | Input (INPUT_PULLUP) |
| D6 | Latch Supervisor | Input (INPUT_PULLUP) |
| D4 | IR Receiver | Input (INPUT_PULLUP) |
| Configurable | Status LED | Output (optional; disabled by default) |

## Configuration

Default values live in `main/lock_config.h`. Matching Kconfig entries live in
`main/Kconfig.projbuild` for native ESP-IDF workflows.

Configurable values include:
- `FORGEKEY_LOCK_SOLENOID_PIN` - Solenoid control pin (default: 0)
- `FORGEKEY_LOCK_REED_PIN` - Reed switch pin (default: 1)
- `FORGEKEY_LOCK_MORTISE_PIN` - Mortise switch pin (default: 5)
- `FORGEKEY_LOCK_LATCH_SUPERVISOR_PIN` - Latch supervisor pin (default: 6)
- `FORGEKEY_LOCK_IR_BEAM_PIN` - IR breakbeam pin (default: 4)
- `FORGEKEY_LOCK_STATUS_LED_PIN` - Optional local status LED pin (default: -1/off)
- `FORGEKEY_LOCK_SOLENOID_PULSE_MS` - Solenoid pulse duration (default: 1500)
- `FORGEKEY_LOCK_TELEMETRY_INTERVAL_MS` - Telemetry interval (default: 10000)

## MQTT Protocol

**Subscribe:**
- `forgekey/{mac}/command` - Operator + lock commands.
- `forgekey/{mac}/config` - Configuration updates.

**Publish:**
- `forgekey/{mac}/status` - Command acks and operator-visible events.
- `forgekey/{mac}/cabinet_lock/status` - Periodic telemetry.

### Signed command envelope

All commands must carry a signed envelope with replay protection. Safety-sensitive
commands (`unlock`, `lockout`, `clear_lockout`, `init_ack`, `emergency_unlock`,
`commission`, `restart`) also require operator identity in `actor`.

JWT form:

```json
{
  "cmd": "lockout",
  "command_id": "cmd_lockout_001",
  "issued_at": 1715150000,
  "expires_at": 1715150030,
  "nonce": "base64url-128-bit-random",
  "actor": "operator:ian",
  "jwt": "header.payload.signature"
}
```

Detached ES256 form:

```json
{
  "cmd": "lockout",
  "command_id": "cmd_lockout_001",
  "issued_at": "1715150000",
  "expires_at": "1715150030",
  "nonce": "base64url-128-bit-random",
  "actor": "operator:ian",
  "signature": "base64url-es256-raw-rs"
}
```

The detached signature covers
`cmd + "\n" + command_id + "\n" + issued_at + "\n" + expires_at + "\n" + nonce + "\n" + actor`.

### Command schemas

#### `lockout`

Enter sticky operator lockout. Normal `unlock` is rejected, telemetry reports
`lockout_active: true`, and the optional status LED fast-flashes.

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

#### `clear_lockout`

Clear lockout and return to `SECURE` when reed and latch are secure; otherwise
return to `ALARM`.

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

Acknowledge OMS initialization or complete commissioning. The device evaluates
physical security and transitions to `SECURE` or `ALARM`.

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

Pulse the solenoid immediately, including while lockout is active. The event and
ack include the initiating command ID.

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

Enter commissioning mode for installation or service. Telemetry reports
`commissioning_active: true` and the optional status LED slow-flashes until
`init_ack`.

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

### Acks, telemetry, and events

A command ack is published on `forgekey/{mac}/status`:

```json
{"cmd_ack":"lockout","command_id":"cmd_lockout_001","state":"LOCKOUT","clock_valid":true}
```

Periodic telemetry on `forgekey/{mac}/cabinet_lock/status` includes `state`,
`secure`, `item_present`, `reed_closed`, `latch_locked`, `ir_broken`,
`mortise_active`, `lockout_active`, `commissioning_active`, `tamper_active`,
`last_trigger`, and time fields.

Debounced `tamper`, `mortise_key`, `latch`, `reed`, and `ir_beam` events are
published immediately on `forgekey/{mac}/status` with timestamps, uptime, current
sensor booleans, state, and the most recent accepted command ID.

## Security Notes

1. Provision the OMS command public key before production deployment.
2. JWT and detached signatures are verified with ES256 against the active OMS
   command public key, with command binding, expiry checks, and replay detection.
3. The solenoid pulse is short (1.5s) to prevent coil overheating.
4. After rotating `src/security/firmware_pubkey.h` or `src/security/oms_ca.h`,
   run `python3 esp32c6-lock/scripts/esp32c6-sync-security.py` before building.
