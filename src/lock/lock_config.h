#ifndef FORGEKEY_LOCK_CONFIG_H
#define FORGEKEY_LOCK_CONFIG_H

// Legacy Arduino cabinet-lock prototype configuration.
// The active lock firmware now lives under esp32c6-lock/ and is built via the
// standalone ESP-IDF project there.

// GPIO pin assignments
#ifndef FORGEKEY_LOCK_SOLENOID_PIN
#define FORGEKEY_LOCK_SOLENOID_PIN 0
#endif

#ifndef FORGEKEY_LOCK_REED_PIN
#define FORGEKEY_LOCK_REED_PIN 1
#endif

#ifndef FORGEKEY_LOCK_MORTISE_PIN
#define FORGEKEY_LOCK_MORTISE_PIN 5
#endif

#ifndef FORGEKEY_LOCK_LATCH_SUPERVISOR_PIN
#define FORGEKEY_LOCK_LATCH_SUPERVISOR_PIN 6
#endif

#ifndef FORGEKEY_LOCK_IR_BEAM_PIN
#define FORGEKEY_LOCK_IR_BEAM_PIN 4
#endif

// Solenoid is 12V driven via MOSFET. Active HIGH on the GPIO turns the
// solenoid ON (unlocks).
#define FORGEKEY_LOCK_SOLENOID_ACTIVE HIGH

// Reed switch: closed (door shut) = LOW (pulled to GND), open = HIGH
#define FORGEKEY_LOCK_REED_CLOSED LOW
#define FORGEKEY_LOCK_REED_OPEN HIGH

// Latch supervisor: locked = LOW, unlocked = HIGH
#define FORGEKEY_LOCK_LATCH_LOCKED LOW
#define FORGEKEY_LOCK_LATCH_UNLOCKED HIGH

// IR breakbeam: beam intact (item present) = HIGH, beam broken = LOW
#define FORGEKEY_LOCK_IR_BEAM_INTACT HIGH
#define FORGEKEY_LOCK_IR_BEAM_BROKEN LOW

// Solenoid unlock pulse duration in ms
#ifndef FORGEKEY_LOCK_SOLENOID_PULSE_MS
#define FORGEKEY_LOCK_SOLENOID_PULSE_MS 1500
#endif

// Auto-relock delay after valid unlock (0 = wait for door close)
#ifndef FORGEKEY_LOCK_AUTO_RELOCK_MS
#define FORGEKEY_LOCK_AUTO_RELOCK_MS 0
#endif

// Telemetry publish interval in ms
#ifndef FORGEKEY_LOCK_TELEMETRY_INTERVAL_MS
#define FORGEKEY_LOCK_TELEMETRY_INTERVAL_MS 10000
#endif

// Command timestamp tolerance (seconds)
#ifndef FORGEKEY_LOCK_CMD_TIMESTAMP_TOLERANCE_S
#define FORGEKEY_LOCK_CMD_TIMESTAMP_TOLERANCE_S 60
#endif

// JWT expiry window (seconds)
#ifndef FORGEKEY_LOCK_JWT_EXPIRY_S
#define FORGEKEY_LOCK_JWT_EXPIRY_S 30
#endif

// Shared HMAC-SHA256 key for JWT validation (change in production)
#ifndef FORGEKEY_LOCK_JWT_SECRET
#define FORGEKEY_LOCK_JWT_SECRET "forgekey-lock-secret-change-in-production"
#endif

// Embedded web server port
#ifndef FORGEKEY_LOCK_WEB_PORT
#define FORGEKEY_LOCK_WEB_PORT 80
#endif

// IR beam debounce time (ms)
#ifndef FORGEKEY_LOCK_IR_DEBOUNCE_MS
#define FORGEKEY_LOCK_IR_DEBOUNCE_MS 500
#endif

// Reed switch debounce time (ms)
#ifndef FORGEKEY_LOCK_REED_DEBOUNCE_MS
#define FORGEKEY_LOCK_REED_DEBOUNCE_MS 200
#endif

// Latch supervisor debounce time (ms)
#ifndef FORGEKEY_LOCK_LATCH_DEBOUNCE_MS
#define FORGEKEY_LOCK_LATCH_DEBOUNCE_MS 200
#endif

// Mortise switch debounce time (ms)
#ifndef FORGEKEY_LOCK_MORTISE_DEBOUNCE_MS
#define FORGEKEY_LOCK_MORTISE_DEBOUNCE_MS 200
#endif

// Alarm LED flash rate (ms per phase)
#ifndef FORGEKEY_LOCK_ALARM_FLASH_MS
#define FORGEKEY_LOCK_ALARM_FLASH_MS 200
#endif

#endif
