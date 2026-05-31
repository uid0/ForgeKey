#ifndef WIFI_DESIRED_STATE_H
#define WIFI_DESIRED_STATE_H

#include <Arduino.h>
#include <ArduinoJson.h>

#include "wifi_setup/captive.h"

namespace wifi_desired_state {

// Applies OMS desired-state WiFi updates delivered on forgekey/<mac>/config.
// The update is two-phase: candidates are tested for WiFi association and the
// supplied reachability probe must pass before the new profile set is committed.
bool apply(JsonVariantConst desired,
           WifiSetup::ReachabilityProbe reachabilityProbe,
           String& detail);

}  // namespace wifi_desired_state

#endif
