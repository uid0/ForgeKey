#include "power_manager.h"

#include <Preferences.h>
#include "esp_sleep.h"
#include "esp_system.h"
#include "soc/soc_caps.h"

namespace PowerManager {
namespace {

const char* kNvsNamespace = "power";
const char* kBrownoutKey = "brownouts";
const char* g_sleepPolicy = "always_awake";
bool g_started = false;
uint32_t g_brownoutCount = 0;

void appendEscaped(String& payload, const char* value) {
    if (!value) return;
    for (const char* p = value; *p; ++p) {
        if (*p == '"' || *p == '\\') payload += '\\';
        payload += *p;
    }
}

void appendStringField(String& payload, const char* key, const char* value) {
    payload += "\"";
    payload += key;
    payload += "\":\"";
    appendEscaped(payload, value ? value : "");
    payload += "\"";
}

const char* resetReasonName(esp_reset_reason_t reason) {
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

}  // namespace

void begin() {
    if (g_started) return;
    Preferences prefs;
    prefs.begin(kNvsNamespace, false);
    g_brownoutCount = prefs.getUInt(kBrownoutKey, 0);
    if (esp_reset_reason() == ESP_RST_BROWNOUT && g_brownoutCount < UINT32_MAX) {
        ++g_brownoutCount;
        prefs.putUInt(kBrownoutKey, g_brownoutCount);
    }
    prefs.end();
    g_started = true;
}

void setSleepPolicy(const char* policy) {
    g_sleepPolicy = (policy && *policy) ? policy : "unknown";
}

const char* resetReasonName() {
    return resetReasonName(esp_reset_reason());
}

const char* wakeReasonName() {
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
#if defined(ESP_SLEEP_WAKEUP_GPIO)
#if SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
        case ESP_SLEEP_WAKEUP_GPIO: return "gpio";
#endif
#endif
#if defined(ESP_SLEEP_WAKEUP_UART)
#if SOC_UART_SUPPORT_WAKEUP_INT
        case ESP_SLEEP_WAKEUP_UART: return "uart";
#endif
#endif
        default: return "unknown";
    }
}

uint32_t brownoutCount() {
    begin();
    return g_brownoutCount;
}

int batteryVoltageMv(const BatteryConfig& config) {
    if (config.adcPin < 0 || config.dividerDenominator == 0) return -1;
    const int sensedMv = analogReadMilliVolts(config.adcPin);
    if (sensedMv <= 0) return -1;
    return (int)(((uint64_t)sensedMv * config.dividerNumerator) / config.dividerDenominator);
}

int batteryPercentFromMv(int mv, const BatteryConfig& config) {
    if (mv < 0) return -1;
    if (mv <= config.emptyMv) return 0;
    if (mv >= config.fullMv) return 100;
    return ((mv - config.emptyMv) * 100) / (config.fullMv - config.emptyMv);
}

bool lowBatteryAlarm(const BatteryConfig& config) {
    const int mv = batteryVoltageMv(config);
    return mv >= 0 && mv <= config.lowMv;
}

void appendHealthJson(String& payload, const BatteryConfig& config) {
    begin();
    const int batteryMv = batteryVoltageMv(config);
    const bool lowBattery = batteryMv >= 0 && batteryMv <= config.lowMv;
    const bool brownout = esp_reset_reason() == ESP_RST_BROWNOUT;

    payload += ",\"power\":{";
    appendStringField(payload, "wake_reason", wakeReasonName());
    payload += ",";
    appendStringField(payload, "reset_reason", resetReasonName());
    payload += ",\"brownout_count\":";
    payload += String(brownoutCount());
    payload += ",";
    appendStringField(payload, "source", batteryMv >= 0 ? "battery_adc" : "external_or_unknown");
    payload += ",";
    appendStringField(payload, "sleep_policy", g_sleepPolicy);
    payload += ",\"battery\":{";
    if (batteryMv >= 0) {
        payload += "\"available\":true,\"voltage_mv\":";
        payload += String(batteryMv);
        payload += ",\"percent\":";
        payload += String(batteryPercentFromMv(batteryMv, config));
        payload += ",\"source\":\"adc\"";
    } else {
        payload += "\"available\":false,\"source\":\"unsupported\",\"reason\":\"";
        appendEscaped(payload, config.unavailableReason ? config.unavailableReason : "battery_adc_not_configured");
        payload += "\"";
    }
    payload += "},\"alarms\":{";
    payload += "\"low_battery\":";
    payload += lowBattery ? "true" : "false";
    payload += ",\"brownout\":";
    payload += brownout ? "true" : "false";
    payload += "}}";

    payload += ",\"diagnostics\":{\"power_alarms\":{\"low_battery\":";
    payload += lowBattery ? "true" : "false";
    payload += ",\"brownout\":";
    payload += brownout ? "true" : "false";
    payload += "}}";
}

}  // namespace PowerManager
