#ifndef FORGEKEY_DEVICE_LIFECYCLE_H
#define FORGEKEY_DEVICE_LIFECYCLE_H

#include <Arduino.h>

namespace device_lifecycle {

enum class Action : uint8_t {
    Retire,
    FactoryReset,
    Reprovision,
};

// Execute a signed lifecycle command after CommandValidation has accepted it.
// Publishes a final retained lifecycle/offline state when MQTT is still
// connected, wipes the requested local NVS credentials, emits an ack, then
// reboots into the resulting lifecycle path.
bool handleSignedCommand(Action action, const char* commandId, const char* actor);

const char* actionName(Action action);

}  // namespace device_lifecycle

#endif
