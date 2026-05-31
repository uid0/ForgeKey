#ifndef FORGEKEY_BUILD_METADATA_HELPERS_H
#define FORGEKEY_BUILD_METADATA_HELPERS_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <forgekey_build_metadata.h>

namespace ForgeKeyBuildMetadata {

inline void appendEscaped(String& payload, const char* value) {
    if (!value) return;
    for (const char* p = value; *p; ++p) {
        switch (*p) {
            case '\\': payload += "\\\\"; break;
            case '"': payload += "\\\""; break;
            case '\b': payload += "\\b"; break;
            case '\f': payload += "\\f"; break;
            case '\n': payload += "\\n"; break;
            case '\r': payload += "\\r"; break;
            case '\t': payload += "\\t"; break;
            default: payload += *p; break;
        }
    }
}

inline void appendStringField(String& payload, const char* key, const char* value) {
    payload += "\"";
    payload += key;
    payload += "\":\"";
    appendEscaped(payload, value ? value : "");
    payload += "\"";
}

inline void appendJson(String& payload, const char* fieldName = "build") {
    payload += ",\"";
    payload += fieldName ? fieldName : "build";
    payload += "\":{";
    appendStringField(payload, "id", FORGEKEY_BUILD_ID);
    payload += ",";
    appendStringField(payload, "git_sha", FORGEKEY_GIT_SHA);
    payload += ",";
    appendStringField(payload, "git_short_sha", FIRMWARE_GIT_COMMIT);
    payload += ",\"dirty\":";
    payload += FORGEKEY_GIT_DIRTY ? "true" : "false";
    payload += ",\"timestamp\":";
    payload += String((unsigned long)FIRMWARE_BUILD_TIMESTAMP);
    payload += ",";
    appendStringField(payload, "target_env", FORGEKEY_BUILD_TARGET);
    payload += ",";
    appendStringField(payload, "release_channel", FORGEKEY_RELEASE_CHANNEL);
    payload += ",";
    appendStringField(payload, "signing_key_id", FORGEKEY_SIGNING_KEY_ID);
    payload += "}";
}

inline void addJson(JsonObject root, const char* fieldName = "build") {
    JsonObject build = root[fieldName ? fieldName : "build"].to<JsonObject>();
    build["id"] = FORGEKEY_BUILD_ID;
    build["git_sha"] = FORGEKEY_GIT_SHA;
    build["git_short_sha"] = FIRMWARE_GIT_COMMIT;
    build["dirty"] = (bool)FORGEKEY_GIT_DIRTY;
    build["timestamp"] = (unsigned long)FIRMWARE_BUILD_TIMESTAMP;
    build["target_env"] = FORGEKEY_BUILD_TARGET;
    build["release_channel"] = FORGEKEY_RELEASE_CHANNEL;
    build["signing_key_id"] = FORGEKEY_SIGNING_KEY_ID;
}

}  // namespace ForgeKeyBuildMetadata

#endif
