#ifndef FORGEKEY_POWER_MANAGER_H
#define FORGEKEY_POWER_MANAGER_H

#include <Arduino.h>

namespace PowerManager {

struct BatteryConfig {
    int adcPin;
    uint32_t dividerNumerator;
    uint32_t dividerDenominator;
    uint16_t emptyMv;
    uint16_t fullMv;
    uint16_t lowMv;
    const char* unavailableReason;
};

void begin();
void setSleepPolicy(const char* policy);
void appendHealthJson(String& payload, const BatteryConfig& config);

const char* resetReasonName();
const char* wakeReasonName();
uint32_t brownoutCount();
int batteryVoltageMv(const BatteryConfig& config);
int batteryPercentFromMv(int mv, const BatteryConfig& config);
bool lowBatteryAlarm(const BatteryConfig& config);

}  // namespace PowerManager

#endif
