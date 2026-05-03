// Push-button capability stub. Reserves the slot; detect() returns false
// today. When a button is wired up (typical: external pull-up to a GPIO,
// active-low momentary contact), populate the probe + edge-detection.
// Suggested probe: configure the configured GPIO as INPUT_PULLUP, sample
// twice ~10ms apart, and require a stable HIGH idle state - otherwise the
// pin is floating (no button) and we should not register.
// Topic suffix would be "button_press" (event payload: button id + edge).

#ifndef FORGEKEY_DISABLE_BUTTON

#include "../capability.h"

namespace Button {

bool detectFn();
void setupFn();
void tickFn();

bool detectFn() {
    // TODO: probe FORGEKEY_BUTTON_PIN for stable pull-up idle state.
    return false;
}

void setupFn() {
    // unreachable while detectFn() returns false
}

void tickFn() {
    // unreachable while detectFn() returns false
}

}  // namespace Button

REGISTER_CAPABILITY(button, "button",
                    Button::detectFn,
                    Button::setupFn,
                    Button::tickFn,
                    "button_press")

#endif
