// Status-LED capability. Owns the blink-pattern state machine that used to
// live inline in main.cpp. main() calls StatusLed::requestState() at WiFi /
// MQTT transitions and tick() services the GPIO.

#ifndef FORGEKEY_DISABLE_STATUS_LED

#include "../capability.h"
#include "status_led.h"

namespace StatusLed {

bool detectFn();
void setupFn();
void tickFn();

namespace {

// Onboard LED on XIAO ESP32-S3 is GPIO 21 (orange). Active LOW.
// TODO: when boards without an onboard LED are added, move this to a
// FORGEKEY_STATUS_LED_PIN compile-time override and have detectFn() return
// false if the override is unset.
constexpr int LED_PIN = 21;
constexpr int LED_ON  = LOW;
constexpr int LED_OFF = HIGH;

struct BlinkPattern {
    int onMs;
    int offMs;
    int count;       // 0 = repeat forever
    // Multi-phase pattern support: when phaseCount > 0, phases[0..phaseCount-1]
    // define alternating on/off durations. phases[i] is on for even i, off for
    // odd i. The entire phase sequence repeats as one blink cycle.
    int  phaseCount;
    const int* phases;
};

constexpr BlinkPattern PATTERN_BOOT            = {100, 100, 5};
constexpr BlinkPattern PATTERN_WIFI_CONNECTING = {500, 500, 0};
constexpr BlinkPattern PATTERN_NORMAL          = { 50, 2950, 0};
constexpr BlinkPattern PATTERN_ERROR           = {200, 200, 0};
constexpr BlinkPattern PATTERN_MQTT_CONNECTED  = { 50,  50, 3};

// Provisioning: "long, long, short, long" morse-style pattern (repeat forever)
// Each "long" = 300ms on + 100ms off; "short" = 100ms on + 100ms off;
// 200ms gap between letters; 600ms gap between cycles.
constexpr int PROVISIONING_PHASES[] = {
    300, 100,   // long 1: on, gap
    300, 100,   // long 2: on, gap
    100, 100,   // short:  on, gap
    300, 100,   // long 3:  on, gap
    200, 400,   // inter-letter gap + inter-cycle gap
};
constexpr BlinkPattern PATTERN_PROVISIONING = {0, 0, 0, 11, PROVISIONING_PHASES};

// Operator-triggered identify blink. Symmetric on/off; tunable via
// -DBLINK_PERIOD_MS=NNN. Default 750ms ≈ 1.33 Hz, visible from across a
// workshop.
#ifndef BLINK_PERIOD_MS
#define BLINK_PERIOD_MS 750
#endif
constexpr BlinkPattern PATTERN_BLINK_OVERRIDE = {BLINK_PERIOD_MS, BLINK_PERIOD_MS, 0};

State g_state = State::Boot;
State g_pendingTransition = State::Boot;
bool  g_haveTransition = false;
BlinkPattern g_currentPattern = PATTERN_BOOT;
BlinkPattern g_followupPattern = PATTERN_NORMAL;  // resumes after finite-count pattern
bool g_haveFollowup = false;
unsigned long g_lastToggle = 0;
bool g_ledOnNow = false;
int  g_blinkCount = 0;
bool g_patternComplete = false;

// Multi-phase pattern tracking
int  g_currentPhase = 0;
unsigned long g_phaseStartMs = 0;
unsigned long g_cycleStartMs = 0;
int  g_cycleCount = 0;

const BlinkPattern& patternFor(State s) {
    switch (s) {
        case State::Boot:           return PATTERN_BOOT;
        case State::WifiConnecting: return PATTERN_WIFI_CONNECTING;
        case State::Provisioning:   return PATTERN_PROVISIONING;
        case State::Normal:         return PATTERN_NORMAL;
        case State::Error:          return PATTERN_ERROR;
        case State::MqttConnected:  return PATTERN_MQTT_CONNECTED;
    }
    return PATTERN_NORMAL;
}

void applyPattern(const BlinkPattern& p) {
    g_currentPattern = p;
    g_blinkCount = 0;
    g_patternComplete = false;
    g_currentPhase = 0;
    g_cycleCount = 0;
    g_cycleStartMs = 0;
    g_phaseStartMs = 0;

    if (p.phaseCount > 0) {
        // Multi-phase pattern: first phase determines initial LED state
        g_ledOnNow = (p.phases[0] > 0);
        digitalWrite(LED_PIN, g_ledOnNow ? LED_ON : LED_OFF);
        g_lastToggle = millis();
    } else {
        // Legacy single-phase pattern
        g_ledOnNow = (p.onMs > 0);
        digitalWrite(LED_PIN, g_ledOnNow ? LED_ON : LED_OFF);
        g_lastToggle = millis();
    }
}

}  // namespace

// Message flash override state (visible to triggerMessageFlash outside anonymous namespace)
bool g_messageFlashActive = false;
unsigned long g_flashStartMs = 0;
bool g_flashLedOn = false;

// Operator blink override (visible to triggerMessageFlash outside anonymous namespace)
bool g_blinkOverride = false;
unsigned long g_blinkOverrideExpiresMs = 0;
bool g_blinkOverrideExpiredFlag = false;

void triggerMessageFlash() {
    if (g_blinkOverride) return;
    g_messageFlashActive = true;
    g_flashStartMs = millis();
    g_flashLedOn = true;
    digitalWrite(LED_PIN, LED_ON);
    g_lastToggle = millis();
}

void requestState(State s) {
    // Remember the requested state even when an operator blink override is
    // active, so clearing the override resumes the right pattern.
    g_pendingTransition = s;
    g_haveTransition = true;
}

bool setBlinkOverride(bool on) {
    if (on == g_blinkOverride) {
        if (!on) g_blinkOverrideExpiresMs = 0;
        return false;
    }
    g_blinkOverride = on;
    g_blinkOverrideExpiresMs = 0;  // explicit on/off cancels any timer
    if (on) {
        applyPattern(PATTERN_BLINK_OVERRIDE);
    } else {
        // Resume whatever the underlying state is. If a transition was queued
        // while the override was active, tickFn() will pick it up. Default to
        // Normal so we don't get stuck mid-Boot if the override outlasted the
        // boot phase.
        applyPattern(patternFor(g_state == State::Boot ? State::Normal : g_state));
    }
    return true;
}

bool setBlinkOverrideTimed(unsigned long durationMs) {
    bool wasOff = !g_blinkOverride;
    if (wasOff) {
        g_blinkOverride = true;
        applyPattern(PATTERN_BLINK_OVERRIDE);
    }
    g_blinkOverrideExpiresMs = (durationMs > 0) ? (millis() + durationMs) : 0;
    return wasOff;
}

bool blinkOverrideActive() { return g_blinkOverride; }

bool consumeBlinkOverrideExpired() {
    if (!g_blinkOverrideExpiredFlag) return false;
    g_blinkOverrideExpiredFlag = false;
    return true;
}

bool detectFn() {
    return true;  // always-present onboard LED
}

void setupFn() {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LED_OFF);
    g_state = State::Boot;
    applyPattern(PATTERN_BOOT);
}

void tickFn() {
    // Auto-expire timed identify-blink override before any pattern logic so
    // the resumed pattern picks up immediately.
    if (g_blinkOverride && g_blinkOverrideExpiresMs != 0 &&
        (long)(millis() - g_blinkOverrideExpiresMs) >= 0) {
        g_blinkOverride = false;
        g_blinkOverrideExpiresMs = 0;
        g_blinkOverrideExpiredFlag = true;
        applyPattern(patternFor(g_state == State::Boot ? State::Normal : g_state));
    }

    if (g_haveTransition) {
        g_haveTransition = false;
        g_state = g_pendingTransition;
        if (g_state == State::MqttConnected) {
            // brief flurry, then resume Normal
            g_haveFollowup = true;
            g_followupPattern = PATTERN_NORMAL;
        } else {
            g_haveFollowup = false;
        }
        // Operator blink override beats MQTT-connected/normal transitions —
        // a reconnect during identify shouldn't silently stop the blink.
        if (!g_blinkOverride) {
            applyPattern(patternFor(g_state));
        }
    }

    // Message flash: brief override of the current state pattern (~500ms)
    if (g_messageFlashActive && !g_blinkOverride) {
        unsigned long now = millis();
        if (now - g_flashStartMs >= 500) {
            g_messageFlashActive = false;
            // Resume the state pattern (or operator override if it changed)
            if (g_blinkOverride) {
                applyPattern(PATTERN_BLINK_OVERRIDE);
            } else {
                applyPattern(patternFor(g_state));
            }
        } else {
            // Toggle LED every 100ms during flash
            if (now - g_lastToggle >= 100) {
                g_flashLedOn = !g_flashLedOn;
                digitalWrite(LED_PIN, g_flashLedOn ? LED_ON : LED_OFF);
                g_lastToggle = now;
            }
        }
        return;
    }

    if (g_patternComplete) {
        if (g_haveFollowup) {
            g_haveFollowup = false;
            applyPattern(g_followupPattern);
        }
        return;
    }

    unsigned long now = millis();

    // Handle multi-phase patterns
    if (g_currentPattern.phaseCount > 0) {
        const int* phases = g_currentPattern.phases;
        int phaseDur = phases[g_currentPhase];

        if (now - g_phaseStartMs >= phaseDur) {
            g_currentPhase++;
            if (g_currentPhase >= g_currentPattern.phaseCount) {
                // Completed one full cycle
                g_currentPhase = 0;
                g_cycleCount++;
                if (g_currentPattern.count > 0 && g_cycleCount >= g_currentPattern.count) {
                    g_patternComplete = true;
                    digitalWrite(LED_PIN, LED_OFF);
                    return;
                }
            }
            // Toggle LED state for the new phase
            g_ledOnNow = (g_currentPhase % 2 == 0);
            digitalWrite(LED_PIN, g_ledOnNow ? LED_ON : LED_OFF);
            g_phaseStartMs = now;
            g_lastToggle = now;

            // Track blink count: each on-phase completion is one blink
            if (g_ledOnNow) {
                ++g_blinkCount;
            }
        }
        return;
    }

    // Legacy single-phase blink
    unsigned long interval = g_ledOnNow ? g_currentPattern.onMs : g_currentPattern.offMs;
    if (now - g_lastToggle >= interval) {
        g_ledOnNow = !g_ledOnNow;
        digitalWrite(LED_PIN, g_ledOnNow ? LED_ON : LED_OFF);
        g_lastToggle = now;
        if (!g_ledOnNow) {
            ++g_blinkCount;
            if (g_currentPattern.count > 0 && g_blinkCount >= g_currentPattern.count) {
                g_patternComplete = true;
                digitalWrite(LED_PIN, LED_OFF);
            }
        }
    }
}

}  // namespace StatusLed

REGISTER_CAPABILITY(status_led, "status_led",
                    StatusLed::detectFn,
                    StatusLed::setupFn,
                    StatusLed::tickFn,
                    nullptr)

#else  // FORGEKEY_DISABLE_STATUS_LED — provide no-op stubs so callers compile.

#include "status_led.h"
namespace StatusLed {
void requestState(State) {}
bool setBlinkOverride(bool) { return false; }
bool setBlinkOverrideTimed(unsigned long) { return false; }
bool blinkOverrideActive() { return false; }
bool consumeBlinkOverrideExpired() { return false; }
void triggerMessageFlash() {}
}

#endif
