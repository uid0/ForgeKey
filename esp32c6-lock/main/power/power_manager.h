#ifndef FORGEKEY_LOCK_POWER_MANAGER_H
#define FORGEKEY_LOCK_POWER_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "cJSON.h"

typedef struct {
    int adc_pin;
    uint32_t divider_numerator;
    uint32_t divider_denominator;
    uint16_t empty_mv;
    uint16_t full_mv;
    uint16_t low_mv;
    const char* unavailable_reason;
} forgekey_power_battery_config_t;

void forgekey_power_init(void);
void forgekey_power_set_sleep_policy(const char* policy);
void forgekey_power_add_health_json(cJSON* root, const forgekey_power_battery_config_t* config);
const char* forgekey_power_reset_reason_name(void);
const char* forgekey_power_wake_reason_name(void);
uint32_t forgekey_power_brownout_count(void);
int forgekey_power_battery_voltage_mv(const forgekey_power_battery_config_t* config);
int forgekey_power_battery_percent_from_mv(int mv, const forgekey_power_battery_config_t* config);
bool forgekey_power_low_battery_alarm(const forgekey_power_battery_config_t* config);

#endif
