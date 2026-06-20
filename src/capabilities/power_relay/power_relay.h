#ifndef FORGEKEY_CAPABILITIES_POWER_RELAY_H
#define FORGEKEY_CAPABILITIES_POWER_RELAY_H

#include <Arduino.h>
#include <ArduinoJson.h>

namespace PowerRelay {

constexpr uint8_t kChannelCount = 2;

void appendHealthJson(String& out);
void appendUsageJson(String& out);
bool setChannel(uint8_t channel, bool on, String& detail);
bool setFromCommand(JsonVariantConst doc, const char* commandId, String& ackJson);
bool channelState(uint8_t channel);

}  // namespace PowerRelay

#endif
