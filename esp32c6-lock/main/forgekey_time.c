#include "forgekey_time.h"

#include <sys/time.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"

static const char* TAG = "FK_TIME";
static const time_t MIN_PLAUSIBLE_EPOCH = 1704067200; /* 2024-01-01T00:00:00Z */
static uint32_t s_max_sync_age_s = FORGEKEY_TIME_DEFAULT_MAX_SYNC_AGE_S;
static volatile uint32_t s_last_sync_uptime_ms = 0;
static volatile bool s_ntp_synced = false;

static uint32_t uptime_ms_now(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool epoch_plausible(time_t epoch) {
    return epoch >= MIN_PLAUSIBLE_EPOCH;
}

static void time_sync_cb(struct timeval* tv) {
    (void)tv;
    s_last_sync_uptime_ms = uptime_ms_now();
    s_ntp_synced = true;
    ESP_LOGI(TAG, "NTP time synchronized");
}

void forgekey_time_begin(uint32_t max_sync_age_s) {
    s_max_sync_age_s = max_sync_age_s ? max_sync_age_s : FORGEKEY_TIME_DEFAULT_MAX_SYNC_AGE_S;
    setenv("TZ", "UTC0", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.nist.gov");
    esp_sntp_set_time_sync_notification_cb(time_sync_cb);
    esp_sntp_init();
    forgekey_time_tick();
}

void forgekey_time_tick(void) {
    time_t now = time(NULL);
    if (epoch_plausible(now) && !s_ntp_synced) {
        s_last_sync_uptime_ms = uptime_ms_now();
        s_ntp_synced = true;
    }
}

forgekey_time_status_t forgekey_time_status(void) {
    forgekey_time_tick();
    uint32_t now_ms = uptime_ms_now();
    time_t now_epoch = time(NULL);
    uint32_t age_s = 0;
    if (s_ntp_synced && s_last_sync_uptime_ms != 0) {
        age_s = (now_ms - s_last_sync_uptime_ms) / 1000UL;
    }
    bool valid = epoch_plausible(now_epoch) && s_ntp_synced && age_s <= s_max_sync_age_s;
    forgekey_time_status_t status = {
        .clock_valid = valid,
        .ntp_synced = s_ntp_synced,
        .epoch_time = now_epoch,
        .monotonic_uptime_ms = now_ms,
        .last_sync_age_s = age_s,
    };
    return status;
}

bool forgekey_time_clock_valid(void) {
    return forgekey_time_status().clock_valid;
}

time_t forgekey_time_epoch_now(void) {
    return time(NULL);
}

uint32_t forgekey_time_uptime_ms(void) {
    return uptime_ms_now();
}

void forgekey_time_add_json(cJSON* root) {
    if (!root) return;
    forgekey_time_status_t status = forgekey_time_status();
    cJSON_AddBoolToObject(root, "clock_valid", status.clock_valid);
    cJSON_AddBoolToObject(root, "ntp_synced", status.ntp_synced);
    cJSON_AddNumberToObject(root, "epoch_time", (double)status.epoch_time);
    cJSON_AddNumberToObject(root, "uptime_ms", status.monotonic_uptime_ms);
    cJSON_AddNumberToObject(root, "last_ntp_sync_age_s", status.last_sync_age_s);
}
