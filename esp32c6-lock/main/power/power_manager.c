#include "power_manager.h"

#include <limits.h>

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "soc/soc_caps.h"
#include "nvs.h"
#include "esp_adc/adc_oneshot.h"

static const char* TAG = "POWER";
static const char* k_nvs_namespace = "power";
static const char* k_brownout_key = "brownouts";
static bool s_started = false;
static uint32_t s_brownout_count = 0;
static const char* s_sleep_policy = "always_awake";

static const char* reset_reason_name(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "power_on";
        case ESP_RST_EXT: return "external";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt_watchdog";
        case ESP_RST_TASK_WDT: return "task_watchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "sdio";
        default: return "unknown";
    }
}

void forgekey_power_init(void) {
    if (s_started) return;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(k_nvs_namespace, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        uint32_t count = 0;
        if (nvs_get_u32(nvs, k_brownout_key, &count) == ESP_OK) {
            s_brownout_count = count;
        }
        if (esp_reset_reason() == ESP_RST_BROWNOUT && s_brownout_count < UINT32_MAX) {
            ++s_brownout_count;
            nvs_set_u32(nvs, k_brownout_key, s_brownout_count);
            nvs_commit(nvs);
        }
        nvs_close(nvs);
    } else {
        ESP_LOGW(TAG, "power NVS unavailable: %s", esp_err_to_name(err));
    }
    s_started = true;
}

void forgekey_power_set_sleep_policy(const char* policy) {
    s_sleep_policy = (policy && policy[0]) ? policy : "unknown";
}

const char* forgekey_power_reset_reason_name(void) {
    return reset_reason_name(esp_reset_reason());
}

const char* forgekey_power_wake_reason_name(void) {
    switch (esp_sleep_get_wakeup_cause()) {
        case ESP_SLEEP_WAKEUP_UNDEFINED: return "power_on_or_reset";
        case ESP_SLEEP_WAKEUP_EXT0: return "ext0";
        case ESP_SLEEP_WAKEUP_EXT1: return "ext1";
        case ESP_SLEEP_WAKEUP_TIMER: return "timer";
#if SOC_TOUCH_SENSOR_SUPPORTED
        case ESP_SLEEP_WAKEUP_TOUCHPAD: return "touchpad";
#endif
#if SOC_ULP_SUPPORTED
        case ESP_SLEEP_WAKEUP_ULP: return "ulp";
#endif
#if SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
        case ESP_SLEEP_WAKEUP_GPIO: return "gpio";
#endif
#if SOC_UART_SUPPORT_WAKEUP_INT
        case ESP_SLEEP_WAKEUP_UART: return "uart";
#endif
        default: return "unknown";
    }
}

uint32_t forgekey_power_brownout_count(void) {
    forgekey_power_init();
    return s_brownout_count;
}

int forgekey_power_battery_voltage_mv(const forgekey_power_battery_config_t* config) {
    if (!config || config->adc_pin < 0 || config->divider_denominator == 0) return -1;

    adc_unit_t unit = ADC_UNIT_1;
    adc_channel_t channel = ADC_CHANNEL_0;
    if (adc_oneshot_io_to_channel(config->adc_pin, &unit, &channel) != ESP_OK) {
        return -1;
    }

    adc_oneshot_unit_handle_t adc = NULL;
    adc_oneshot_unit_init_cfg_t init_cfg = {.unit_id = unit};
    if (adc_oneshot_new_unit(&init_cfg, &adc) != ESP_OK) {
        return -1;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t err = adc_oneshot_config_channel(adc, channel, &chan_cfg);
    int raw = 0;
    if (err == ESP_OK) {
        err = adc_oneshot_read(adc, channel, &raw);
    }
    adc_oneshot_del_unit(adc);
    if (err != ESP_OK || raw <= 0) {
        return -1;
    }

    /* ESP-IDF calibration is intentionally not required for bring-up: convert
     * the 12-bit raw code against the configured 3.3 V ADC-safe divider node.
     * Board manifests carry the divider ratio, and production boards can tighten
     * this with calibration eFuses or per-board trim later. */
    int sensed_mv = (raw * 3300) / 4095;
    return (int)(((uint64_t)sensed_mv * config->divider_numerator) /
                 config->divider_denominator);
}

int forgekey_power_battery_percent_from_mv(int mv, const forgekey_power_battery_config_t* config) {
    if (!config || mv < 0) return -1;
    if (mv <= config->empty_mv) return 0;
    if (mv >= config->full_mv) return 100;
    return ((mv - config->empty_mv) * 100) / (config->full_mv - config->empty_mv);
}

bool forgekey_power_low_battery_alarm(const forgekey_power_battery_config_t* config) {
    int mv = forgekey_power_battery_voltage_mv(config);
    return mv >= 0 && config && mv <= config->low_mv;
}

void forgekey_power_add_health_json(cJSON* root, const forgekey_power_battery_config_t* config) {
    if (!root) return;
    forgekey_power_init();
    int battery_mv = forgekey_power_battery_voltage_mv(config);
    bool low_battery = battery_mv >= 0 && config && battery_mv <= config->low_mv;

    cJSON* power = cJSON_CreateObject();
    cJSON_AddStringToObject(power, "wake_reason", forgekey_power_wake_reason_name());
    cJSON_AddStringToObject(power, "reset_reason", forgekey_power_reset_reason_name());
    cJSON_AddNumberToObject(power, "brownout_count", forgekey_power_brownout_count());
    cJSON_AddStringToObject(power, "source", battery_mv >= 0 ? "battery_adc" : "external_or_unknown");
    cJSON_AddStringToObject(power, "sleep_policy", s_sleep_policy);

    cJSON* battery = cJSON_CreateObject();
    if (battery_mv >= 0 && config) {
        cJSON_AddBoolToObject(battery, "available", true);
        cJSON_AddNumberToObject(battery, "voltage_mv", battery_mv);
        cJSON_AddNumberToObject(battery, "percent", forgekey_power_battery_percent_from_mv(battery_mv, config));
        cJSON_AddStringToObject(battery, "source", "adc");
    } else {
        cJSON_AddBoolToObject(battery, "available", false);
        cJSON_AddStringToObject(battery, "source", "unsupported");
        cJSON_AddStringToObject(battery, "reason", config && config->unavailable_reason ? config->unavailable_reason : "battery_adc_not_configured");
    }
    cJSON_AddItemToObject(power, "battery", battery);

    cJSON* alarms = cJSON_CreateObject();
    cJSON_AddBoolToObject(alarms, "low_battery", low_battery);
    cJSON_AddBoolToObject(alarms, "brownout", esp_reset_reason() == ESP_RST_BROWNOUT);
    cJSON_AddItemToObject(power, "alarms", alarms);
    cJSON_AddItemToObject(root, "power", power);

    cJSON* diagnostics = cJSON_CreateObject();
    cJSON* power_alarms = cJSON_CreateObject();
    cJSON_AddBoolToObject(power_alarms, "low_battery", low_battery);
    cJSON_AddBoolToObject(power_alarms, "brownout", esp_reset_reason() == ESP_RST_BROWNOUT);
    cJSON_AddItemToObject(diagnostics, "power_alarms", power_alarms);
    cJSON_AddItemToObject(root, "diagnostics", diagnostics);
}
