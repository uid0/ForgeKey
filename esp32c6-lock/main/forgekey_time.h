#ifndef FORGEKEY_TIME_H
#define FORGEKEY_TIME_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "cJSON.h"

#define FORGEKEY_TIME_DEFAULT_MAX_SYNC_AGE_S (24UL * 60UL * 60UL)

typedef struct {
    bool clock_valid;
    bool ntp_synced;
    time_t epoch_time;
    uint32_t monotonic_uptime_ms;
    uint32_t last_sync_age_s;
} forgekey_time_status_t;

void forgekey_time_begin(uint32_t max_sync_age_s);
void forgekey_time_tick(void);
forgekey_time_status_t forgekey_time_status(void);
bool forgekey_time_clock_valid(void);
time_t forgekey_time_epoch_now(void);
uint32_t forgekey_time_uptime_ms(void);
void forgekey_time_add_json(cJSON* root);

#endif
