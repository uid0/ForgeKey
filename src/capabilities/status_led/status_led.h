#ifndef FORGEKEY_CAPABILITIES_STATUS_LED_H
#define FORGEKEY_CAPABILITIES_STATUS_LED_H

#include <Arduino.h>

// Onboard-LED status capability. Always-detected on the XIAO ESP32-S3 (the
// orange LED on GPIO 21 is built in to the board), so detect() returns true
// unconditionally. Future hardware (e.g. an external NeoPixel, or boards
// without an onboard LED) can override the detect probe.
//
// main() drives state transitions; the capability owns the timer state and
// blink-pattern lookup so the loop in main.cpp doesn't have to.
namespace StatusLed {

enum class State {
    Boot,            // initial fast blink on power-up
    WifiConnecting,  // slow blink while WiFi/portal is in progress
    Normal,          // short blink every few seconds (steady-state OK)
    Error,           // fast blink (something's wrong)
    MqttConnected,   // brief flurry on (re)connect, then back to Normal
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

}

#endif
