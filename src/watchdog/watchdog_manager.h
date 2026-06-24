#ifndef FORGEKEY_WATCHDOG_MANAGER_H
#define FORGEKEY_WATCHDOG_MANAGER_H

#include <Arduino.h>

namespace ForgeKeyWatchdog {

enum class Subsystem : uint8_t {
    WiFi = 0,
    MQTT,
    Camera,
    OTA,
    BLE,
    Sensors,
    LockStateMachine,
    Count,
};

using RecoveryCallback = bool (*)(Subsystem subsystem, const char* reason);

void begin();
void setRecoveryCallback(RecoveryCallback callback);
void setEnabled(Subsystem subsystem, bool enabled);
void markHealthy(Subsystem subsystem);
void markBusy(Subsystem subsystem);
void markIdle(Subsystem subsystem);
void suspend(const char* reason);
void resume();
void tick(bool mqttConnected);
void appendHealthJson(String& payload);
const char* subsystemName(Subsystem subsystem);

class CriticalSection {
public:
    explicit CriticalSection(const char* reason) { suspend(reason); }
    ~CriticalSection() { resume(); }
};

}  // namespace ForgeKeyWatchdog

#endif
