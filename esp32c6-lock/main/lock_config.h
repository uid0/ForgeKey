/*
 * Lock configuration for ESP32-C6 build
 * 
 * Defaults can be overridden via sdkconfig or Kconfig.
 */

#ifndef FORGEKEY_LOCK_CONFIG_H
#define FORGEKEY_LOCK_CONFIG_H

/* GPIO pin assignments */
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

/* Solenoid active level */
#define FORGEKEY_LOCK_SOLENOID_ACTIVE 1

/* Reed switch states */
#define FORGEKEY_LOCK_REED_CLOSED 0
#define FORGEKEY_LOCK_REED_OPEN 1

/* Latch supervisor states */
#define FORGEKEY_LOCK_LATCH_LOCKED 0
#define FORGEKEY_LOCK_LATCH_UNLOCKED 1

/* IR beam states */
#define FORGEKEY_LOCK_IR_BEAM_INTACT 1
#define FORGEKEY_LOCK_IR_BEAM_BROKEN 0

/* Solenoid pulse duration (ms) */
#ifndef FORGEKEY_LOCK_SOLENOID_PULSE_MS
#define FORGEKEY_LOCK_SOLENOID_PULSE_MS 1500
#endif

/* Auto-relock delay (ms, 0 = disabled) */
#ifndef FORGEKEY_LOCK_AUTO_RELOCK_MS
#define FORGEKEY_LOCK_AUTO_RELOCK_MS 0
#endif

/* Telemetry interval (ms) */
#ifndef FORGEKEY_LOCK_TELEMETRY_INTERVAL_MS
#define FORGEKEY_LOCK_TELEMETRY_INTERVAL_MS 10000
#endif

/* Command timestamp tolerance (seconds) */
#ifndef FORGEKEY_LOCK_CMD_TIMESTAMP_TOLERANCE_S
#define FORGEKEY_LOCK_CMD_TIMESTAMP_TOLERANCE_S 60
#endif

/* JWT expiry window (seconds) */
#ifndef FORGEKEY_LOCK_JWT_EXPIRY_S
#define FORGEKEY_LOCK_JWT_EXPIRY_S 30
#endif

/* JWT shared secret */
#ifndef FORGEKEY_LOCK_JWT_SECRET
#define FORGEKEY_LOCK_JWT_SECRET "forgekey-lock-secret-change-in-production"
#endif

/* Web server port */
#ifndef FORGEKEY_LOCK_WEB_PORT
#define FORGEKEY_LOCK_WEB_PORT 80
#endif

/* Debounce times (ms) */
#ifndef FORGEKEY_LOCK_IR_DEBOUNCE_MS
#define FORGEKEY_LOCK_IR_DEBOUNCE_MS 500
#endif

#ifndef FORGEKEY_LOCK_REED_DEBOUNCE_MS
#define FORGEKEY_LOCK_REED_DEBOUNCE_MS 200
#endif

#ifndef FORGEKEY_LOCK_LATCH_DEBOUNCE_MS
#define FORGEKEY_LOCK_LATCH_DEBOUNCE_MS 200
#endif

#ifndef FORGEKEY_LOCK_MORTISE_DEBOUNCE_MS
#define FORGEKEY_LOCK_MORTISE_DEBOUNCE_MS 200
#endif

/* Alarm LED flash rate (ms) */
#ifndef FORGEKEY_LOCK_ALARM_FLASH_MS
#define FORGEKEY_LOCK_ALARM_FLASH_MS 200
#endif

#endif /* FORGEKEY_LOCK_CONFIG_H */
