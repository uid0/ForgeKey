#include "watchdog_manager.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_idf_version.h"
#include "esp_wifi.h"

#include "mqtt_handler.h"

static const char* TAG = "WDT";

#define TASK_WDT_TIMEOUT_MS 30000U
#define WARNING_INTERVAL_MS 60000ULL
#define REBOOT_AFTER_MS (15ULL * 60ULL * 1000ULL)

typedef struct {
    const char* name;
    uint32_t timeout_ms;
    uint64_t last_healthy_ms;
    uint64_t first_overdue_ms;
    uint64_t last_warning_ms;
    uint32_t warning_count;
    uint32_t recovery_count;
    bool busy;
    bool enabled;
    bool overdue;
} watchdog_state_t;

static watchdog_state_t s_states[FORGEKEY_WATCHDOG_COUNT] = {
    [FORGEKEY_WATCHDOG_WIFI] = {"wifi", 120000, 0, 0, 0, 0, 0, false, true, false},
    [FORGEKEY_WATCHDOG_MQTT] = {"mqtt", 180000, 0, 0, 0, 0, 0, false, true, false},
    [FORGEKEY_WATCHDOG_CAMERA] = {"camera", 180000, 0, 0, 0, 0, 0, false, true, false},
    [FORGEKEY_WATCHDOG_OTA] = {"ota", 600000, 0, 0, 0, 0, 0, false, true, false},
    [FORGEKEY_WATCHDOG_BLE] = {"ble", 300000, 0, 0, 0, 0, 0, false, true, false},
    [FORGEKEY_WATCHDOG_SENSORS] = {"sensors", 300000, 0, 0, 0, 0, 0, false, true, false},
    [FORGEKEY_WATCHDOG_LOCK_STATE_MACHINE] = {"lock_state_machine", 30000, 0, 0, 0, 0, 0, false, true, false},
};

static forgekey_watchdog_recovery_cb_t s_recovery_cb = NULL;
static uint8_t s_suspend_depth = 0;
static const char* s_suspend_reason = NULL;
static esp_reset_reason_t s_reset_reason = ESP_RST_UNKNOWN;
static bool s_task_wdt_enabled = false;
static bool s_started = false;
static uint64_t s_boot_ms = 0;

static uint64_t now_ms(void) {
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

static watchdog_state_t* state_for(forgekey_watchdog_subsystem_t subsystem) {
    if (subsystem < 0 || subsystem >= FORGEKEY_WATCHDOG_COUNT) return NULL;
    return &s_states[subsystem];
}

static void reset_task_wdt(void) {
    if (s_task_wdt_enabled) esp_task_wdt_reset();
}

static void publish_warning(forgekey_watchdog_subsystem_t subsystem,
                            const watchdog_state_t* state,
                            const char* action) {
    const char* topic = mqtt_handler_get_status_topic();
    if (!topic || !topic[0] || !state) return;
    char payload[320];
    snprintf(payload, sizeof(payload),
             "{\"event\":\"watchdog_warning\",\"subsystem\":\"%s\","
             "\"action\":\"%s\",\"age_ms\":%llu,\"warnings\":%lu,"
             "\"recoveries\":%lu,\"safe_guard\":%s%s%s%s}",
             state->name,
             action ? action : "warn",
             (unsigned long long)(now_ms() - state->last_healthy_ms),
             (unsigned long)state->warning_count,
             (unsigned long)state->recovery_count,
             s_suspend_depth ? "true" : "false",
             s_suspend_reason ? ",\"safe_guard_reason\":\"" : "",
             s_suspend_reason ? s_suspend_reason : "",
             s_suspend_reason ? "\"" : "");
    (void)subsystem;
    mqtt_handler_publish_queued(topic, payload, -1, 0, 0, true);
}

const char* forgekey_watchdog_subsystem_name(forgekey_watchdog_subsystem_t subsystem) {
    watchdog_state_t* state = state_for(subsystem);
    return state ? state->name : "unknown";
}

void forgekey_watchdog_begin(void) {
    uint64_t now = now_ms();
    s_boot_ms = now;
    s_reset_reason = esp_reset_reason();
    for (int i = 0; i < FORGEKEY_WATCHDOG_COUNT; ++i) {
        s_states[i].last_healthy_ms = now;
        s_states[i].first_overdue_ms = 0;
        s_states[i].last_warning_ms = 0;
        s_states[i].warning_count = 0;
        s_states[i].recovery_count = 0;
        s_states[i].busy = false;
        s_states[i].overdue = false;
    }
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t config = {
        .timeout_ms = TASK_WDT_TIMEOUT_MS,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    esp_err_t err = esp_task_wdt_init(&config);
#else
    esp_err_t err = esp_task_wdt_init(TASK_WDT_TIMEOUT_MS / 1000, true);
#endif
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        esp_err_t add_err = esp_task_wdt_add(NULL);
        s_task_wdt_enabled = (add_err == ESP_OK || add_err == ESP_ERR_INVALID_STATE);
    }
    ESP_LOGI(TAG, "watchdog started task_wdt=%d reset_reason=%d", (int)s_task_wdt_enabled, (int)s_reset_reason);
    s_started = true;
}

void forgekey_watchdog_set_recovery_callback(forgekey_watchdog_recovery_cb_t callback) {
    s_recovery_cb = callback;
}

void forgekey_watchdog_mark_healthy(forgekey_watchdog_subsystem_t subsystem) {
    watchdog_state_t* state = state_for(subsystem);
    if (!state) return;
    state->last_healthy_ms = now_ms();
    state->first_overdue_ms = 0;
    state->overdue = false;
}

void forgekey_watchdog_mark_busy(forgekey_watchdog_subsystem_t subsystem) {
    watchdog_state_t* state = state_for(subsystem);
    if (!state) return;
    state->busy = true;
    forgekey_watchdog_mark_healthy(subsystem);
}

void forgekey_watchdog_mark_idle(forgekey_watchdog_subsystem_t subsystem) {
    watchdog_state_t* state = state_for(subsystem);
    if (!state) return;
    state->busy = false;
    forgekey_watchdog_mark_healthy(subsystem);
}

void forgekey_watchdog_suspend(const char* reason) {
    if (s_suspend_depth < 255) s_suspend_depth++;
    s_suspend_reason = reason;
    reset_task_wdt();
}

void forgekey_watchdog_resume(void) {
    if (s_suspend_depth > 0) s_suspend_depth--;
    if (s_suspend_depth == 0) {
        s_suspend_reason = NULL;
        uint64_t now = now_ms();
        for (int i = 0; i < FORGEKEY_WATCHDOG_COUNT; ++i) {
            if (s_states[i].busy) s_states[i].last_healthy_ms = now;
        }
        reset_task_wdt();
    }
}

void forgekey_watchdog_tick(bool mqtt_connected) {
    if (!s_started) forgekey_watchdog_begin();
    uint64_t now = now_ms();
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) forgekey_watchdog_mark_healthy(FORGEKEY_WATCHDOG_WIFI);
    if (mqtt_connected) forgekey_watchdog_mark_healthy(FORGEKEY_WATCHDOG_MQTT);

    bool any_unsafe_overdue = false;
    for (int i = 0; i < FORGEKEY_WATCHDOG_COUNT; ++i) {
        watchdog_state_t* state = &s_states[i];
        if (!state->enabled || state->busy) continue;
        uint64_t age = now - state->last_healthy_ms;
        if (age <= state->timeout_ms) continue;
        if (state->first_overdue_ms == 0) state->first_overdue_ms = now;
        state->overdue = true;
        any_unsafe_overdue = true;
        if (state->last_warning_ms == 0 || now - state->last_warning_ms >= WARNING_INTERVAL_MS) {
            state->last_warning_ms = now;
            state->warning_count++;
            bool recovered = false;
            if (s_suspend_depth == 0 && s_recovery_cb) {
                recovered = s_recovery_cb((forgekey_watchdog_subsystem_t)i, "health_timeout");
            }
            if (recovered) {
                state->recovery_count++;
                forgekey_watchdog_mark_healthy((forgekey_watchdog_subsystem_t)i);
                publish_warning((forgekey_watchdog_subsystem_t)i, state, "subsystem_restarted");
            } else {
                publish_warning((forgekey_watchdog_subsystem_t)i, state,
                                s_suspend_depth ? "deferred_safe_guard" : "warn");
            }
        }
        if (s_suspend_depth == 0 && now - state->first_overdue_ms >= REBOOT_AFTER_MS) {
            publish_warning((forgekey_watchdog_subsystem_t)i, state, "device_reboot");
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
    }
    if (!any_unsafe_overdue || s_suspend_depth > 0) reset_task_wdt();
}

void forgekey_watchdog_add_health_json(cJSON* root) {
    if (!root) return;
    cJSON* watchdog = cJSON_CreateObject();
    cJSON_AddBoolToObject(watchdog, "task_wdt_enabled", s_task_wdt_enabled);
    cJSON_AddNumberToObject(watchdog, "last_reset_reason", (int)s_reset_reason);
    cJSON_AddNumberToObject(watchdog, "uptime_ms", (double)(now_ms() - s_boot_ms));
    cJSON_AddBoolToObject(watchdog, "safe_guard_active", s_suspend_depth > 0);
    if (s_suspend_reason) cJSON_AddStringToObject(watchdog, "safe_guard_reason", s_suspend_reason);
    cJSON* subsystems = cJSON_CreateObject();
    uint64_t now = now_ms();
    for (int i = 0; i < FORGEKEY_WATCHDOG_COUNT; ++i) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "age_ms", (double)(now - s_states[i].last_healthy_ms));
        cJSON_AddNumberToObject(item, "timeout_ms", s_states[i].timeout_ms);
        cJSON_AddBoolToObject(item, "overdue", s_states[i].overdue);
        cJSON_AddBoolToObject(item, "busy", s_states[i].busy);
        cJSON_AddNumberToObject(item, "warnings", s_states[i].warning_count);
        cJSON_AddNumberToObject(item, "recoveries", s_states[i].recovery_count);
        cJSON_AddItemToObject(subsystems, s_states[i].name, item);
    }
    cJSON_AddItemToObject(watchdog, "subsystems", subsystems);
    cJSON_AddItemToObject(root, "watchdog", watchdog);
}
