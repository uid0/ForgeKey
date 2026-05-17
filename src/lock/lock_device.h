#ifndef FORGEKEY_LOCK_DEVICE_H
#define FORGEKEY_LOCK_DEVICE_H

#include <Arduino.h>
#include "lock_config.h"

// Non-blocking state machine for the cabinet lock.
// All state transitions are driven by tick() called from the main loop.
// No blocking delays — GPIO reads and transitions use millis() timing.

namespace LockDevice {

enum class State {
    INITIALIZING,  // Blinking: connecting WiFi -> NTP -> MQTT
    SECURE,        // Red light. Latch engaged. Reed = Closed AND Latch = Locked.
    UNLOCKED,      // Green light. Latch disengaged. Waiting for door opening.
    ACCESSING,     // Green light. Door Reed = Open. IR beam monitoring item removal.
    ALARM,         // Rapid Red/Green flash. Door open without valid signed command or mortise key.
};

// Initialize GPIO pins and internal state. Call once in setup().
void begin();

// Main state machine tick. Call every loop iteration (~10ms).
// Handles:
//   - GPIO debouncing and state transitions
//   - Solenoid pulse timing
//   - Alarm LED flashing
//   - Telemetry change detection
void tick();

// Get the current lock state.
State getState();

// Get the human-readable state name for logging/web display.
const char* stateName(State s);

// Validate an incoming signed command token. Returns true if the token is valid:
//   - ES256 signature verifies against the OMS command public key
//   - Token payload contains the expected MAC / command claims when present
//   - Token has not expired (per FORGEKEY_LOCK_JWT_EXPIRY_S)
// token: the full JWT string (header.payload.signature)
// timestamp: the timestamp from the MQTT command payload (for logging)
// expectedCmd: bind the JWT's `cmd` claim to this verb (e.g. "unlock"). Pass
//   nullptr / "" to accept any verb.
bool validateJwt(const char* token, long timestamp, const char* expectedCmd);

// Handle an incoming unlock command with a signed token.
// Returns true if the token was valid and the solenoid was pulsed.
bool handleUnlockCommand(const char* token, long timestamp);

// Get the last trigger type that caused a state change.
enum class Trigger {
    NONE,
    SIGNED_COMMAND,
    MORTISE,
    AUTO_UNLOCK,
    DOOR_CLOSE,
    ALARM_TIMEOUT
};
Trigger getLastTrigger();

// Get current telemetry values (computed from GPIO state).
struct Telemetry {
    bool secure;       // reed_closed AND latch_locked
    bool reed_closed;  // raw reed switch state
    bool latch_locked; // raw latch supervisor state
    bool ir_broken;    // raw IR beam state
    bool mortise_active; // raw mortise switch state
    unsigned long uptime_ms;
};
Telemetry getTelemetry();

// Get the MAC address string for telemetry.
String getMacAddress();

}  // namespace LockDevice

#endif
