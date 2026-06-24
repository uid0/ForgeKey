#include "watchdog_manager.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_idf_version.h>

#if __has_include(<esp_task_wdt.h>)
#include <esp_task_wdt.h>
#define FORGEKEY_HAS_TASK_WDT 1
#else
#define FORGEKEY_HAS_TASK_WDT 0
#endif

#include "mqtt/mqtt_client.h"

namespace ForgeKeyWatchdog {
namespace {

constexpr unsigned long kTaskWatchdogTimeoutMs = 30000UL;
constexpr unsigned long kWarningIntervalMs = 60000UL;
constexpr unsigned long kRebootAfterMs = 15UL * 60UL * 1000UL;

struct SubsystemState {
    const char* name;
    unsigned long timeoutMs;
    unsigned long lastHealthyMs;
    unsigned long firstOverdueMs;
    unsigned long lastWarningMs;
    uint32_t warningCount;
    uint32_t recoveryCount;
    bool busy;
    bool enabled;
    bool overdue;
};

SubsystemState g_subsystems[] = {
    {"wifi", 120000UL, 0, 0, 0, 0, 0, false, true, false},
    {"mqtt", 180000UL, 0, 0, 0, 0, 0, false, true, false},
    {"camera", 180000UL, 0, 0, 0, 0, 0, false, true, false},
    {"ota", 600000UL, 0, 0, 0, 0, 0, false, true, false},
    {"ble", 300000UL, 0, 0, 0, 0, 0, false, true, false},
    {"sensors", 300000UL, 0, 0, 0, 0, 0, false, true, false},
    {"lock_state_machine", 30000UL, 0, 0, 0, 0, 0, false, true, false},
};

RecoveryCallback g_recoveryCallback = nullptr;
bool g_taskWdtEnabled = false;
bool g_started = false;
uint8_t g_suspendDepth = 0;
const char* g_suspendReason = nullptr;
esp_reset_reason_t g_resetReason = ESP_RST_UNKNOWN;
unsigned long g_bootMs = 0;

SubsystemState* stateFor(Subsystem subsystem) {
    size_t index = static_cast<size_t>(subsystem);
    if (index >= static_cast<size_t>(Subsystem::Count)) return nullptr;
    return &g_subsystems[index];
}

void publishWarning(Subsystem subsystem, const SubsystemState& state, const char* action) {
    JsonDocument doc;
    doc["event"] = "watchdog_warning";
    doc["subsystem"] = state.name;
    doc["action"] = action ? action : "warn";
    doc["age_ms"] = (unsigned long)(millis() - state.lastHealthyMs);
    doc["warnings"] = state.warningCount;
    doc["recoveries"] = state.recoveryCount;
    doc["safe_guard"] = g_suspendDepth > 0;
    if (g_suspendReason) doc["safe_guard_reason"] = g_suspendReason;
    String payload;
    serializeJson(doc, payload);
    mqttClient.enqueueStatus(payload.c_str(), true);
}

void enableTaskWatchdog() {
#if FORGEKEY_HAS_TASK_WDT
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t config = {
        .timeout_ms = kTaskWatchdogTimeoutMs,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    esp_err_t err = esp_task_wdt_init(&config);
#else
    esp_err_t err = esp_task_wdt_init(kTaskWatchdogTimeoutMs / 1000UL, true);
#endif
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        esp_err_t addErr = esp_task_wdt_add(nullptr);
        g_taskWdtEnabled = (addErr == ESP_OK || addErr == ESP_ERR_INVALID_STATE);
    }
#endif
}

void resetTaskWatchdog() {
#if FORGEKEY_HAS_TASK_WDT
    if (g_taskWdtEnabled) esp_task_wdt_reset();
#endif
}

}  // namespace

const char* subsystemName(Subsystem subsystem) {
    SubsystemState* state = stateFor(subsystem);
    return state ? state->name : "unknown";
}

void begin() {
    unsigned long now = millis();
    g_bootMs = now;
    g_resetReason = esp_reset_reason();
    for (auto& state : g_subsystems) {
        state.lastHealthyMs = now;
        state.firstOverdueMs = 0;
        state.lastWarningMs = 0;
        state.warningCount = 0;
        state.recoveryCount = 0;
        state.busy = false;
        state.overdue = false;
    }
    enableTaskWatchdog();
    g_started = true;
}

void setRecoveryCallback(RecoveryCallback callback) {
    g_recoveryCallback = callback;
}

void setEnabled(Subsystem subsystem, bool enabled) {
    SubsystemState* state = stateFor(subsystem);
    if (!state) return;
    state->enabled = enabled;
    if (enabled) {
        markHealthy(subsystem);
    } else {
        state->firstOverdueMs = 0;
        state->overdue = false;
        state->busy = false;
    }
}

void markHealthy(Subsystem subsystem) {
    SubsystemState* state = stateFor(subsystem);
    if (!state) return;
    state->lastHealthyMs = millis();
    state->firstOverdueMs = 0;
    state->overdue = false;
}

void markBusy(Subsystem subsystem) {
    SubsystemState* state = stateFor(subsystem);
    if (!state) return;
    state->busy = true;
    markHealthy(subsystem);
}

void markIdle(Subsystem subsystem) {
    SubsystemState* state = stateFor(subsystem);
    if (!state) return;
    state->busy = false;
    markHealthy(subsystem);
}

void suspend(const char* reason) {
    if (g_suspendDepth < 255) g_suspendDepth++;
    g_suspendReason = reason;
    resetTaskWatchdog();
}

void resume() {
    if (g_suspendDepth > 0) g_suspendDepth--;
    if (g_suspendDepth == 0) {
        g_suspendReason = nullptr;
        unsigned long now = millis();
        for (auto& state : g_subsystems) {
            if (state.busy) state.lastHealthyMs = now;
        }
        resetTaskWatchdog();
    }
}

void tick(bool mqttConnected) {
    if (!g_started) begin();
    unsigned long now = millis();

    if (WiFi.status() == WL_CONNECTED) markHealthy(Subsystem::WiFi);
    if (mqttConnected) markHealthy(Subsystem::MQTT);

    bool anyUnsafeOverdue = false;
    for (size_t i = 0; i < static_cast<size_t>(Subsystem::Count); ++i) {
        SubsystemState& state = g_subsystems[i];
        if (!state.enabled || state.busy) continue;
        unsigned long age = now - state.lastHealthyMs;
        if (age <= state.timeoutMs) continue;

        if (state.firstOverdueMs == 0) state.firstOverdueMs = now;
        state.overdue = true;
        anyUnsafeOverdue = true;

        if (state.lastWarningMs == 0 || now - state.lastWarningMs >= kWarningIntervalMs) {
            state.lastWarningMs = now;
            state.warningCount++;
            bool recovered = false;
            if (g_suspendDepth == 0 && g_recoveryCallback) {
                recovered = g_recoveryCallback(static_cast<Subsystem>(i), "health_timeout");
            }
            if (recovered) {
                state.recoveryCount++;
                markHealthy(static_cast<Subsystem>(i));
                publishWarning(static_cast<Subsystem>(i), state, "subsystem_restarted");
            } else {
                publishWarning(static_cast<Subsystem>(i), state,
                               g_suspendDepth > 0 ? "deferred_safe_guard" : "warn");
            }
        }

        if (g_suspendDepth == 0 && now - state.firstOverdueMs >= kRebootAfterMs) {
            publishWarning(static_cast<Subsystem>(i), state, "device_reboot");
            delay(100);
            ESP.restart();
        }
    }

    if (!anyUnsafeOverdue || g_suspendDepth > 0) {
        resetTaskWatchdog();
    }
}

void appendHealthJson(String& payload) {
    payload += ",\"watchdog\":{";
    payload += "\"task_wdt_enabled\":";
    payload += g_taskWdtEnabled ? "true" : "false";
    payload += ",\"last_reset_reason\":";
    payload += String((int)g_resetReason);
    payload += ",\"uptime_ms\":";
    payload += String(millis() - g_bootMs);
    payload += ",\"safe_guard_active\":";
    payload += (g_suspendDepth > 0) ? "true" : "false";
    if (g_suspendReason) {
        payload += ",\"safe_guard_reason\":\"";
        payload += g_suspendReason;
        payload += "\"";
    }
    payload += ",\"subsystems\":{";
    bool first = true;
    unsigned long now = millis();
    for (const auto& state : g_subsystems) {
        if (!first) payload += ",";
        first = false;
        payload += "\"";
        payload += state.name;
        payload += "\":{";
        payload += "\"age_ms\":";
        payload += String(now - state.lastHealthyMs);
        payload += ",\"timeout_ms\":";
        payload += String(state.timeoutMs);
        payload += ",\"overdue\":";
        payload += state.overdue ? "true" : "false";
        payload += ",\"busy\":";
        payload += state.busy ? "true" : "false";
        payload += ",\"warnings\":";
        payload += String(state.warningCount);
        payload += ",\"recoveries\":";
        payload += String(state.recoveryCount);
        payload += "}";
    }
    payload += "}}";
}

}  // namespace ForgeKeyWatchdog
