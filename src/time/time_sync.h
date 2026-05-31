#ifndef FORGEKEY_TIME_SYNC_H
#define FORGEKEY_TIME_SYNC_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>

namespace ForgeKeyTime {

constexpr uint32_t kDefaultMaxSyncAgeS = 24UL * 60UL * 60UL;

struct Status {
    bool clockValid;
    bool ntpSynced;
    time_t epochTime;
    uint32_t monotonicUptimeMs;
    uint32_t lastSyncAgeS;
};

void begin(uint32_t maxSyncAgeS = kDefaultMaxSyncAgeS);
void tick();
Status status();
bool clockValid();
time_t epochNow();
uint32_t uptimeMs();
uint32_t lastSyncAgeS();
void addJson(JsonObject obj);
void appendJson(String& payload);

}  // namespace ForgeKeyTime

#endif
