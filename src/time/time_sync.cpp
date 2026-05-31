#include "time_sync.h"

#include <esp_sntp.h>
#include <stdlib.h>

namespace ForgeKeyTime {
namespace {
constexpr time_t kMinimumPlausibleEpoch = 1704067200;  // 2024-01-01T00:00:00Z
uint32_t g_maxSyncAgeS = kDefaultMaxSyncAgeS;
volatile uint32_t g_lastSyncUptimeMs = 0;
volatile bool g_ntpSynced = false;

void onTimeSync(struct timeval*) {
    g_lastSyncUptimeMs = millis();
    g_ntpSynced = true;
}

bool epochPlausible(time_t value) {
    return value >= kMinimumPlausibleEpoch;
}

uint32_t ageFromLastSyncMs(uint32_t nowMs) {
    uint32_t lastMs = g_lastSyncUptimeMs;
    if (!g_ntpSynced || lastMs == 0) return UINT32_MAX;
    return (nowMs - lastMs) / 1000UL;
}

}  // namespace

void begin(uint32_t maxSyncAgeS) {
    g_maxSyncAgeS = maxSyncAgeS;
    setenv("TZ", "UTC0", 1);
    tzset();
    sntp_set_time_sync_notification_cb(onTimeSync);
    configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");
    tick();
}

void tick() {
    time_t now = time(nullptr);
    if (epochPlausible(now) && !g_ntpSynced) {
        // Covers a successful blocking getLocalTime()/SNTP set before the
        // callback is observed, while still recording the monotonic baseline.
        g_lastSyncUptimeMs = millis();
        g_ntpSynced = true;
    }
}

Status status() {
    tick();
    uint32_t nowMs = millis();
    time_t nowEpoch = time(nullptr);
    uint32_t ageS = ageFromLastSyncMs(nowMs);
    bool valid = epochPlausible(nowEpoch) && g_ntpSynced && ageS <= g_maxSyncAgeS;
    return Status{valid, g_ntpSynced, nowEpoch, nowMs, ageS == UINT32_MAX ? 0 : ageS};
}

bool clockValid() { return status().clockValid; }
time_t epochNow() { return time(nullptr); }
uint32_t uptimeMs() { return millis(); }
uint32_t lastSyncAgeS() { return status().lastSyncAgeS; }

void addJson(JsonObject obj) {
    Status s = status();
    obj["clock_valid"] = s.clockValid;
    obj["ntp_synced"] = s.ntpSynced;
    obj["epoch_time"] = static_cast<long>(s.epochTime);
    obj["uptime_ms"] = s.monotonicUptimeMs;
    obj["last_ntp_sync_age_s"] = s.lastSyncAgeS;
}

void appendJson(String& payload) {
    Status s = status();
    payload += ",\"clock_valid\":";
    payload += s.clockValid ? "true" : "false";
    payload += ",\"ntp_synced\":";
    payload += s.ntpSynced ? "true" : "false";
    payload += ",\"epoch_time\":";
    payload += String(static_cast<long>(s.epochTime));
    payload += ",\"uptime_ms\":";
    payload += String(s.monotonicUptimeMs);
    payload += ",\"last_ntp_sync_age_s\":";
    payload += String(s.lastSyncAgeS);
}

}  // namespace ForgeKeyTime
