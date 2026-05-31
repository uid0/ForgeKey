#ifndef FORGEKEY_LOCK_WATCHDOG_MANAGER_H
#define FORGEKEY_LOCK_WATCHDOG_MANAGER_H

#include <stdbool.h>
#include "cJSON.h"

typedef enum {
    FORGEKEY_WATCHDOG_WIFI = 0,
    FORGEKEY_WATCHDOG_MQTT,
    FORGEKEY_WATCHDOG_CAMERA,
    FORGEKEY_WATCHDOG_OTA,
    FORGEKEY_WATCHDOG_BLE,
    FORGEKEY_WATCHDOG_SENSORS,
    FORGEKEY_WATCHDOG_LOCK_STATE_MACHINE,
    FORGEKEY_WATCHDOG_COUNT,
} forgekey_watchdog_subsystem_t;

typedef bool (*forgekey_watchdog_recovery_cb_t)(forgekey_watchdog_subsystem_t subsystem,
                                                const char* reason);

void forgekey_watchdog_begin(void);
void forgekey_watchdog_set_recovery_callback(forgekey_watchdog_recovery_cb_t callback);
void forgekey_watchdog_mark_healthy(forgekey_watchdog_subsystem_t subsystem);
void forgekey_watchdog_mark_busy(forgekey_watchdog_subsystem_t subsystem);
void forgekey_watchdog_mark_idle(forgekey_watchdog_subsystem_t subsystem);
void forgekey_watchdog_suspend(const char* reason);
void forgekey_watchdog_resume(void);
void forgekey_watchdog_tick(bool mqtt_connected);
void forgekey_watchdog_add_health_json(cJSON* root);
const char* forgekey_watchdog_subsystem_name(forgekey_watchdog_subsystem_t subsystem);

#endif
