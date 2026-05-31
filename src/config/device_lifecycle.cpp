#include "device_lifecycle.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "../mqtt/mqtt_client.h"
#include "../provisioning/register.h"
#include "../time/time_sync.h"

namespace device_lifecycle {
namespace {
constexpr const char* kLifecycleNvsNamespace = "fk_lifecycle";
constexpr const char* kLifecycleStateKey = "state";
constexpr const char* kWifiProfilesNamespace = "fk_wifi";

void persistLifecycleState(const char* state) {
    Preferences prefs;
    if (!prefs.begin(kLifecycleNvsNamespace, false)) return;
    prefs.putString(kLifecycleStateKey, state ? state : "unknown");
    prefs.end();
}

void wipeWifiProfiles() {
    Preferences prefs;
    if (prefs.begin(kWifiProfilesNamespace, false)) {
        prefs.clear();
        prefs.end();
    }
    // Also drop credentials held by the ESP WiFi driver / Arduino WiFi stack.
    WiFi.disconnect(true, true);
    esp_wifi_restore();
}

void publishFinalState(const char* state, const char* reason, const char* commandId, const char* actor) {
    JsonDocument doc;
    doc["online"] = false;
    doc["lifecycle_state"] = state ? state : "unknown";
    doc["reason"] = reason ? reason : "lifecycle_command";
    doc["command_id"] = commandId ? commandId : "";
    doc["actor"] = actor ? actor : "";
    ForgeKeyTime::addJson(doc.as<JsonObject>());
    String payload;
    serializeJson(doc, payload);
    mqttClient.publishStateJson(payload.c_str());
}

void publishAck(const char* action, const char* commandId, bool ok, const char* detail) {
    JsonDocument ack;
    ack["cmd_ack"] = action ? action : "";
    ack["command_id"] = commandId ? commandId : "";
    ack["ok"] = ok;
    if (detail && *detail) ack["detail"] = detail;
    String payload;
    serializeJson(ack, payload);
    mqttClient.publishStatus(payload.c_str());
}

void rebootSoon() {
    delay(750);
    ESP.restart();
}
}  // namespace

const char* actionName(Action action) {
    switch (action) {
        case Action::Retire:
            return "retire";
        case Action::FactoryReset:
            return "factory_reset";
        case Action::Reprovision:
            return "reprovision";
        default:
            return "unknown";
    }
}

bool handleSignedCommand(Action action, const char* commandId, const char* actor) {
    const char* state = "unknown";
    const char* detail = "";
    switch (action) {
        case Action::Retire:
            state = "retired";
            detail = "identity_cleared_wifi_retained";
            break;
        case Action::FactoryReset:
            state = "factory_reset";
            detail = "all_local_credentials_wiped";
            break;
        case Action::Reprovision:
            state = "reprovisioning";
            detail = "identity_cleared_wifi_retained";
            break;
        default:
            return false;
    }

    persistLifecycleState(state);
    publishFinalState(state, actionName(action), commandId, actor);
    publishAck(actionName(action), commandId, true, detail);
    delay(250);

    if (action == Action::FactoryReset) {
        provisioning.wipeCredentials(true, false);
        wipeWifiProfiles();
    } else {
        provisioning.wipeCredentials(false, false);
    }

    rebootSoon();
    return true;
}

}  // namespace device_lifecycle
