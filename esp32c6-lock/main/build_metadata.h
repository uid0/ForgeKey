#ifndef FORGEKEY_LOCK_BUILD_METADATA_H
#define FORGEKEY_LOCK_BUILD_METADATA_H

#include "cJSON.h"
#include "forgekey_build_metadata.h"

static inline void forgekey_build_metadata_set_item(cJSON* object, const char* name, cJSON* item) {
    if (!object || !name || !item) return;
    cJSON_DeleteItemFromObjectCaseSensitive(object, name);
    cJSON_AddItemToObject(object, name, item);
}

static inline void forgekey_build_metadata_add_json(cJSON* root) {
    if (!root) return;
    cJSON* build = cJSON_GetObjectItemCaseSensitive(root, "build");
    if (!cJSON_IsObject(build)) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "build");
        build = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "build", build);
    }
    forgekey_build_metadata_set_item(build, "id", cJSON_CreateString(FORGEKEY_BUILD_ID));
    forgekey_build_metadata_set_item(build, "git_sha", cJSON_CreateString(FORGEKEY_GIT_SHA));
    forgekey_build_metadata_set_item(build, "git_short_sha", cJSON_CreateString(FIRMWARE_GIT_COMMIT));
    forgekey_build_metadata_set_item(build, "dirty", cJSON_CreateBool(FORGEKEY_GIT_DIRTY));
    forgekey_build_metadata_set_item(build, "timestamp", cJSON_CreateNumber(FIRMWARE_BUILD_TIMESTAMP));
    forgekey_build_metadata_set_item(build, "target_env", cJSON_CreateString(FORGEKEY_BUILD_TARGET));
    forgekey_build_metadata_set_item(build, "release_channel", cJSON_CreateString(FORGEKEY_RELEASE_CHANNEL));
    forgekey_build_metadata_set_item(build, "signing_key_id", cJSON_CreateString(FORGEKEY_SIGNING_KEY_ID));
}

#endif
