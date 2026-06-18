#ifndef FORGEKEY_CAPABILITIES_STATUS_LED_H
#define FORGEKEY_CAPABILITIES_STATUS_LED_H

#include <Arduino.h>

// Onboard-LED status capability. It implements the shared ForgeKey operator
// state semantics documented in docs/STATUS_SEMANTICS.md so people-counter,
// temperature, ePaper, and lock-class firmware use the same visual language.
//
// main() drives state transitions; the capability owns the timer state and
// blink-pattern lookup so the loop in main.cpp doesn't have to.
namespace StatusLed {

enum class State {
    Booting,        // initial fast blink on power-up
    Provisioning,   // captive portal / WiFi / OMS enrollment in progress
    Connected,      // steady-state OK
    Degraded,       // running, but broker/network/sensor path needs attention
    Ota,            // OTA download / verify / reboot window
    Error,          // blocking fault that needs operator attention
    Identify,       // operator locate override
    Retired,        // signed retire command accepted; identity is being cleared
    FactoryReset,   // signed factory reset accepted; local credentials are being wiped

    // Backward-compatible aliases for older call sites and downstream forks.
    Boot = Booting,
    WifiConnecting = Provisioning,
    Normal = Connected,
    MqttConnected = Connected,
};

// Request a state transition. Safe to call from anywhere; the capability's
// tick() is what actually drives the GPIO. If the capability is not active
// (LED disabled at compile time), this is a no-op.
void requestState(State s);

// Operator-triggered "identify me" override. While true, the LED blinks at
// the BLINK_PERIOD_MS cadence and ignores requestState() transitions. The
// underlying state is preserved and resumes when the override is cleared
// (e.g. an MQTT-reconnect burst that fires while the operator is staring at
// the device should not silently stop the identify blink). Returns true if
// the override actually changed (so callers can decide whether to publish a
// status echo); false on no-op (already in the requested mode) or when the
// capability is disabled.
bool setBlinkOverride(bool on);
// Same as setBlinkOverride(true), but auto-clears after durationMs. A second
// call while a timer is active extends/replaces the deadline. Pass 0 for
// no timer (equivalent to setBlinkOverride(true)). Returns true if the
// override transitioned from off->on; false if it was already on (timer is
// still updated in that case).
bool setBlinkOverrideTimed(unsigned long durationMs);
bool blinkOverrideActive();
// One-shot check: returns true if the timed override expired since the last
// call (and clears the flag). main() polls this each loop to publish a
// {"blink":"off"} status echo when the identify timer auto-clears.
bool consumeBlinkOverrideExpired();

// Trigger a brief LED flash to indicate a message was sent (MQTT publish or
// HTTP POST). The flash temporarily overrides the current state pattern for
// ~500ms, then resumes the pattern that was active when it was triggered.
// Safe to call from anywhere; ignored if the operator blink override is
// active (identify mode takes priority).
void triggerMessageFlash();

}

#endif
