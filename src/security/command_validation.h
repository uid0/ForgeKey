#ifndef FORGEKEY_COMMAND_VALIDATION_H
#define FORGEKEY_COMMAND_VALIDATION_H

#include <Arduino.h>
#include <ArduinoJson.h>

namespace CommandValidation {

struct Result {
    bool ok = false;
    const char* error = "invalid_command";
    const char* detail = nullptr;
    String commandId;
    String nonce;
    String actor;
};

void begin();
Result validate(JsonVariantConst doc, const String& deviceMac);
void rememberAccepted(const Result& result);
const char* canonicalError(const Result& result);

}  // namespace CommandValidation

#endif
