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

## Configuration

Default values live in `main/lock_config.h`. Matching Kconfig entries live in
`main/Kconfig.projbuild` for native ESP-IDF workflows.

Configurable values include:
- `FORGEKEY_LOCK_SOLENOID_PIN` - Solenoid control pin (default: 0)
- `FORGEKEY_LOCK_REED_PIN` - Reed switch pin (default: 1)
- `FORGEKEY_LOCK_MORTISE_PIN` - Mortise switch pin (default: 5)
- `FORGEKEY_LOCK_LATCH_SUPERVISOR_PIN` - Latch supervisor pin (default: 6)
- `FORGEKEY_LOCK_IR_BEAM_PIN` - IR breakbeam pin (default: 4)
- `FORGEKEY_LOCK_SOLENOID_PULSE_MS` - Solenoid pulse duration (default: 1500)
- `FORGEKEY_LOCK_TELEMETRY_INTERVAL_MS` - Telemetry interval (default: 10000)

## MQTT Protocol

**Subscribe:**
- `forgekey/{mac}/command` - Operator + lock commands. JSON envelope `{"cmd": "<verb>", "jwt": "<ES256>", "command_id": "...", ...}`. Supported verbs: `unlock`, `status`, `ping`, `restart`. Reserved (acked as `not_implemented`): `lockout`, `clear_lockout`, `init_ack`.
- `forgekey/{mac}/config` - Configuration updates

**Publish:**
- `forgekey/{mac}/status` - Command acks (`{"cmd_ack": "<verb>", "command_id": "...", "state"/"error": "..."}`) and operator-visible state changes
- `forgekey/{mac}/cabinet_lock/status` - Telemetry

## Security Notes

1. Change `FORGEKEY_LOCK_JWT_SECRET` before production deployment
2. JWT validation already includes HMAC-SHA256 signature verification via mbedTLS
3. The solenoid pulse is short (1.5s) to prevent coil overheating
4. After rotating `src/security/firmware_pubkey.h` or `src/security/oms_ca.h`,
   run `python3 esp32c6-lock/scripts/esp32c6-sync-security.py` before building
