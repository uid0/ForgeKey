#ifndef FORGEKEY_SUPPORT_LOCAL_STATUS_WEB_H
#define FORGEKEY_SUPPORT_LOCAL_STATUS_WEB_H

#include <Arduino.h>

namespace LocalStatusWeb {

using StatusProvider = String (*)();

// Starts the optional local status page for powered Arduino-class devices.
// ePaper/deep-sleep builds compile to no-ops to avoid awake-time and power cost.
void begin(const String& mac, StatusProvider provider);
void tick();
bool active();

}  // namespace LocalStatusWeb

#endif
