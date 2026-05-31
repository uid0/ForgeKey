#include "lock_board_manifest.h"
#include "../lock_config.h"

#include "esp_log.h"
#include <stdio.h>

static const char* TAG = "LOCK_BOARD";

static const lock_pin_claim_t PIN_CLAIMS[] = {
    {FORGEKEY_LOCK_SOLENOID_PIN, "cabinet_lock", "solenoid", "none", true, false},
    {FORGEKEY_LOCK_REED_PIN, "cabinet_lock", "reed_switch", "pullup", false, false},
    {FORGEKEY_LOCK_IR_BEAM_PIN, "cabinet_lock", "ir_beam", "pullup", false, false},
    {FORGEKEY_LOCK_MORTISE_PIN, "cabinet_lock", "mortise", "pullup", false, false},
    {FORGEKEY_LOCK_LATCH_SUPERVISOR_PIN, "cabinet_lock", "latch_supervisor", "pullup", false, false},
};

static const int BOOT_STRAP_PINS[] = {8, 9, 15};
static const int ADC_PINS[] = {0, 1, 2, 3, 4, 5, 6, 7};

#ifndef CONFIG_FORGEKEY_LOCK_BATTERY_ADC_PIN
#define CONFIG_FORGEKEY_LOCK_BATTERY_ADC_PIN -1
#endif
#ifndef CONFIG_FORGEKEY_LOCK_BATTERY_DIVIDER_NUM
#define CONFIG_FORGEKEY_LOCK_BATTERY_DIVIDER_NUM 2
#endif
#ifndef CONFIG_FORGEKEY_LOCK_BATTERY_DIVIDER_DEN
#define CONFIG_FORGEKEY_LOCK_BATTERY_DIVIDER_DEN 1
#endif
#ifndef CONFIG_FORGEKEY_LOCK_BATTERY_EMPTY_MV
#define CONFIG_FORGEKEY_LOCK_BATTERY_EMPTY_MV 3300
#endif
#ifndef CONFIG_FORGEKEY_LOCK_BATTERY_FULL_MV
#define CONFIG_FORGEKEY_LOCK_BATTERY_FULL_MV 4200
#endif
#ifndef CONFIG_FORGEKEY_LOCK_BATTERY_LOW_MV
#define CONFIG_FORGEKEY_LOCK_BATTERY_LOW_MV 3450
#endif

static const lock_board_manifest_t BOARD = {
    .id = "esp32c6-lock",
    .name = "ESP32-C6 cabinet lock",
    .pins = PIN_CLAIMS,
    .pin_count = sizeof(PIN_CLAIMS) / sizeof(PIN_CLAIMS[0]),
    .boot_strap_pins = BOOT_STRAP_PINS,
    .boot_strap_pin_count = sizeof(BOOT_STRAP_PINS) / sizeof(BOOT_STRAP_PINS[0]),
    .adc_pins = ADC_PINS,
    .adc_pin_count = sizeof(ADC_PINS) / sizeof(ADC_PINS[0]),
    .battery = {
        .adc_pin = CONFIG_FORGEKEY_LOCK_BATTERY_ADC_PIN,
        .divider_numerator = CONFIG_FORGEKEY_LOCK_BATTERY_DIVIDER_NUM,
        .divider_denominator = CONFIG_FORGEKEY_LOCK_BATTERY_DIVIDER_DEN,
        .empty_mv = CONFIG_FORGEKEY_LOCK_BATTERY_EMPTY_MV,
        .full_mv = CONFIG_FORGEKEY_LOCK_BATTERY_FULL_MV,
        .low_mv = CONFIG_FORGEKEY_LOCK_BATTERY_LOW_MV,
        .unavailable_reason = "battery_adc_not_configured"
    },
};

static bool pin_in_list(int gpio, const int* pins, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        if (pins[i] == gpio) return true;
    }
    return false;
}

const lock_board_manifest_t* lock_board_manifest_current(void) {
    return &BOARD;
}

const forgekey_power_battery_config_t* lock_board_manifest_battery_config(void) {
    return &BOARD.battery;
}

bool lock_board_manifest_check(void) {
    bool ok = true;
    ESP_LOGI(TAG, "manifest=%s (%s), pins=%u", BOARD.id, BOARD.name, BOARD.pin_count);
    for (unsigned i = 0; i < BOARD.pin_count; ++i) {
        const lock_pin_claim_t* a = &BOARD.pins[i];
        if (a->gpio < 0) {
            ESP_LOGE(TAG, "%s/%s has invalid GPIO%d", a->owner, a->signal, a->gpio);
            ok = false;
            continue;
        }
        if (pin_in_list(a->gpio, BOARD.boot_strap_pins, BOARD.boot_strap_pin_count)) {
            ESP_LOGE(TAG, "%s/%s uses boot strapping GPIO%d", a->owner, a->signal, a->gpio);
            ok = false;
        }
        for (unsigned j = i + 1; j < BOARD.pin_count; ++j) {
            const lock_pin_claim_t* b = &BOARD.pins[j];
            if (a->gpio == b->gpio) {
                ESP_LOGE(TAG, "GPIO%d conflict: %s/%s vs %s/%s", a->gpio,
                         a->owner, a->signal, b->owner, b->signal);
                ok = false;
            }
        }
    }
    return ok;
}

void lock_board_manifest_add_health_json(cJSON* root) {
    if (!root) return;
    cJSON* hw = cJSON_CreateObject();
    cJSON_AddStringToObject(hw, "board_id", BOARD.id);
    cJSON_AddStringToObject(hw, "board_name", BOARD.name);

    cJSON* pins = cJSON_CreateArray();
    for (unsigned i = 0; i < BOARD.pin_count; ++i) {
        const lock_pin_claim_t* p = &BOARD.pins[i];
        cJSON* pin = cJSON_CreateObject();
        cJSON_AddNumberToObject(pin, "gpio", p->gpio);
        cJSON_AddStringToObject(pin, "owner", p->owner);
        cJSON_AddStringToObject(pin, "signal", p->signal);
        cJSON_AddStringToObject(pin, "pull", p->pull);
        cJSON_AddBoolToObject(pin, "output", p->output);
        cJSON_AddBoolToObject(pin, "unsafe", p->unsafe);
        cJSON_AddItemToArray(pins, pin);
    }
    cJSON_AddItemToObject(hw, "pins", pins);

    cJSON* battery_sense = cJSON_CreateObject();
    cJSON_AddNumberToObject(battery_sense, "adc_pin", BOARD.battery.adc_pin);
    char ratio[24];
    snprintf(ratio, sizeof(ratio), "%u/%u", (unsigned)BOARD.battery.divider_numerator,
             (unsigned)BOARD.battery.divider_denominator);
    cJSON_AddStringToObject(battery_sense, "divider_ratio", ratio);
    cJSON_AddNumberToObject(battery_sense, "low_mv", BOARD.battery.low_mv);
    cJSON_AddBoolToObject(battery_sense, "configured", BOARD.battery.adc_pin >= 0);
    cJSON_AddItemToObject(hw, "battery_sense", battery_sense);

    cJSON* active = cJSON_CreateArray();
    cJSON_AddItemToArray(active, cJSON_CreateString("cabinet_lock"));
    cJSON_AddItemToObject(hw, "active_capabilities", active);
    cJSON_AddItemToObject(hw, "skipped_capabilities", cJSON_CreateArray());
    cJSON_AddItemToObject(root, "hardware", hw);
}
