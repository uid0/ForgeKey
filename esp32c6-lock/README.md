# ESP32-C6 Lock Device

ESP-IDF native build for the ForgeKey cabinet-lock on Seeed Studio XIAO ESP32-C6.

## Build Instructions

```bash
# Install ESP-IDF (v5.0+) with ESP32-C6 support
# https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/get-started/index.html

# Set up ESP-IDF environment
. $IDF_PATH/export.sh

# Build
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

Configurable via `sdkconfig`:
- `FORGEKEY_LOCK_SOLENOID_PIN` - Solenoid control pin (default: 0)
- `FORGEKEY_LOCK_REED_PIN` - Reed switch pin (default: 1)
- `FORGEKEY_LOCK_MORTISE_PIN` - Mortise switch pin (default: 5)
- `FORGEKEY_LOCK_LATCH_SUPERVISOR_PIN` - Latch supervisor pin (default: 6)
- `FORGEKEY_LOCK_IR_BEAM_PIN` - IR breakbeam pin (default: 4)
- `FORGEKEY_LOCK_SOLENOID_PULSE_MS` - Solenoid pulse duration (default: 1500)
- `FORGEKEY_LOCK_TELEMETRY_INTERVAL_MS` - Telemetry interval (default: 10000)

## MQTT Protocol

**Subscribe:**
- `cabinets/{mac}/cmd` - Unlock commands (JWT token + timestamp)
- `forgekey/{mac}/config` - Configuration updates

**Publish:**
- `forgekey/{mac}/cabinet_lock/status` - Telemetry

## Security Notes

1. Change `FORGEKEY_LOCK_JWT_SECRET` before production deployment
2. Implement full HMAC-SHA256 JWT verification
3. The solenoid pulse is short (1.5s) to prevent coil overheating
