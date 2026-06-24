#ifndef FORGEKEY_CAPABILITIES_STATUS_MATRIX_H
#define FORGEKEY_CAPABILITIES_STATUS_MATRIX_H

#include <Arduino.h>
#include "../status_led/status_led.h"

namespace StatusMatrix {

void requestState(StatusLed::State s);
void setIdentifyOverride(bool on);
void triggerMessageFlash();
bool setIndicator(const char* indicator, unsigned long durationMs);
void clearIndicator();

}

#endif
