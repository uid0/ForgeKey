#include "wifi_desired_state.h"

namespace wifi_desired_state {

bool apply(JsonVariantConst desired,
           WifiSetup::ReachabilityProbe reachabilityProbe,
           String& detail) {
    return WifiSetup::applyDesiredState(desired, reachabilityProbe, detail);
}

}  // namespace wifi_desired_state
