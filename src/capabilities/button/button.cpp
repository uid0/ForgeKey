// Push-button capability. Activation is manifest-gated so an unpopulated
// header pin is never probed unless the board manifest or build flags claim it.
// The default electrical contract is an active-low momentary switch with a
// pull-up idle state. Topic suffix: "button_press".

#ifndef FORGEKEY_DISABLE_BUTTON

#include "../capability.h"
#include "../../boards/board_manifest.h"
#include <Arduino.h>

namespace Button {

bool detectFn();
void setupFn();
void tickFn();

namespace {
int g_pin = -1;
bool g_lastPressed = false;
unsigned long g_lastEdgeMs = 0;
}

bool detectFn() {
    g_pin = BoardManifest::buttonPin();
    if (g_pin < 0 || !BoardManifest::capabilityAllowed("button")) {
        Serial.println("[CAP/button] skipped: no button pin in board manifest");
        return false;
    }
    pinMode(g_pin, INPUT_PULLUP);
    delay(10);
    const int first = digitalRead(g_pin);
    delay(10);
    const int second = digitalRead(g_pin);
    const bool stableIdle = first == HIGH && second == HIGH;
    if (!stableIdle) {
        Serial.printf("[CAP/button] skipped: GPIO%d did not show stable pull-up idle\n", g_pin);
    }
    return stableIdle;
}

void setupFn() {
    pinMode(g_pin, INPUT_PULLUP);
    g_lastPressed = digitalRead(g_pin) == LOW;
    g_lastEdgeMs = millis();
}

void tickFn() {
    if (g_pin < 0) return;
    const bool pressed = digitalRead(g_pin) == LOW;
    const unsigned long now = millis();
    if (pressed != g_lastPressed && now - g_lastEdgeMs >= 30) {
        g_lastPressed = pressed;
        g_lastEdgeMs = now;
        Serial.printf("[CAP/button] edge=%s gpio=%d\n", pressed ? "pressed" : "released", g_pin);
    }
}

}  // namespace Button

REGISTER_CAPABILITY(button, "button",
                    Button::detectFn,
                    Button::setupFn,
                    Button::tickFn,
                    "button_press")

#endif
