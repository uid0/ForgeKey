#include "credential_rotation.h"

#include <ArduinoJson.h>

#include "provisioning/register.h"

namespace credential_rotation {

String topicFor(const String& mac) {
    return String("forgekey/") + mac + "/config";
}

void onConfigMessage(const char* topic, const uint8_t* payload, unsigned int length) {
    Serial.printf("config: message on %s (%u bytes)\n", topic, length);

    StaticJsonDocument<384> doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
        Serial.printf("config: parse error: %s\n", err.c_str());
        return;
    }

    const char* newToken = doc["provisioning_token"] | "";
    const char* validAfter = doc["valid_after"] | "";

    if (newToken == nullptr || newToken[0] == '\0') {
        Serial.println("config: payload missing provisioning_token — ignoring");
        return;
    }

    String tokenStr(newToken);
    if (provisioning.setProvisioningToken(tokenStr)) {
        // Log a short prefix only — the full token is a secret and the serial
        // console is shoulder-surfable when the device is on a bench.
        String preview = tokenStr.substring(0, 8);
        Serial.printf("config: stored new provisioning token (prefix=%s..., len=%u, valid_after=%s)\n",
                      preview.c_str(), (unsigned)tokenStr.length(), validAfter);
    } else {
        Serial.println("config: NVS write failed; rotation NOT applied");
    }
}

}  // namespace credential_rotation
