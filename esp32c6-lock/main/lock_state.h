/*
 * Lock state machine for ESP32-C6 lock device.
 * Non-blocking state machine with GPIO debouncing, solenoid pulse,
 * and ES256 signed-command verification via mbedtls.
 */

#ifndef FORGEKEY_LOCK_STATE_H
#define FORGEKEY_LOCK_STATE_H

#include <stdbool.h>
#include <stdint.h>

/* Lock states */
typedef enum {
    LOCK_STATE_INITIALIZING,
    LOCK_STATE_SECURE,
    LOCK_STATE_UNLOCKED,
    LOCK_STATE_ACCESSING,
    LOCK_STATE_ALARM,
} lock_state_t;

/* Lock trigger types */
typedef enum {
    LOCK_TRIGGER_NONE,
    LOCK_TRIGGER_SIGNED_COMMAND,
    LOCK_TRIGGER_MORTISE,
    LOCK_TRIGGER_AUTO_UNLOCK,
    LOCK_TRIGGER_DOOR_CLOSE,
    LOCK_TRIGGER_ALARM_TIMEOUT,
} lock_trigger_t;

/* Telemetry data */
typedef struct {
    bool secure;
    bool reed_closed;
    bool latch_locked;
    bool ir_broken;
    bool mortise_active;
    uint32_t uptime_ms;
} lock_telemetry_t;

/* Initialize GPIO pins for lock sensors and solenoid. */
void lock_state_gpio_init(void);

/* Initialize internal state. Call after GPIO init. */
void lock_state_begin(void);

/* Tick the state machine. Call every ~10ms from main loop. */
void lock_state_tick(void);

/* Get current lock state. */
lock_state_t lock_state_get_state(void);

/* Get human-readable state name. */
const char* lock_state_state_name(lock_state_t state);

/* Get the last trigger type. */
lock_trigger_t lock_state_get_last_trigger(void);

/* Get current telemetry. */
lock_telemetry_t lock_state_get_telemetry(void);

/* Handle an incoming unlock command with a signed token.
 * Returns true if token was valid and solenoid was pulsed. */
bool lock_state_handle_unlock(const char* token, long timestamp);

/* Validate a signed command token (header.payload.signature).
 * Performs ES256 signature verification against the OMS command public key.
 * Returns true if valid. */
bool lock_state_validate_signed_command(const char* token, long timestamp);

/* Get MAC address string (caller must free). */
char* lock_state_get_mac_address(void);

/* Set the MAC address (called from main.c after WiFi connects). */
void lock_state_set_mac(const char* mac);

#endif /* FORGEKEY_LOCK_STATE_H */
