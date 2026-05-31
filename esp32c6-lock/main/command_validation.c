#include "command_validation.h"

#include "lock_state.h"
#include "forgekey_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "nvs.h"

static const char* TAG = "CMDSEC";
#define CACHE_SIZE 16
#define VALUE_MAX 72
#define NVS_NS "cmdsec"
#define NVS_KEY "replay"

typedef struct {
    char command_id[VALUE_MAX];
    char nonce[VALUE_MAX];
} replay_entry_t;

static replay_entry_t s_cache[CACHE_SIZE];
static size_t s_count;
static size_t s_next;
static bool s_loaded;

static bool nonempty(const char* v) { return v && v[0] != '\0'; }

static const char* json_string(cJSON* doc, const char* key) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(doc, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

static bool parse_epoch(cJSON* item, time_t* out) {
    if (!item || !out) return false;
    if (cJSON_IsNumber(item)) {
        *out = (time_t)item->valuedouble;
        return *out > 0;
    }
    if (!cJSON_IsString(item) || !nonempty(item->valuestring)) return false;
    char* end = NULL;
    long numeric = strtol(item->valuestring, &end, 10);
    if (end && *end == '\0' && numeric > 0) {
        *out = (time_t)numeric;
        return true;
    }
    struct tm tm_value = {0};
    char* parsed = strptime(item->valuestring, "%Y-%m-%dT%H:%M:%SZ", &tm_value);
    if (!parsed || *parsed != '\0') return false;
    *out = mktime(&tm_value);
    return *out > 0;
}

static bool uses_server_challenge_flow(cJSON* doc) {
    const char* auth_flow = json_string(doc, "auth_flow");
    const char* challenge = json_string(doc, "challenge");
    const char* server_nonce = json_string(doc, "server_nonce");
    return strcmp(auth_flow, "challenge") == 0 || strcmp(auth_flow, "nonce") == 0 ||
           nonempty(challenge) || nonempty(server_nonce);
}

static bool replay_seen(const char* command_id, const char* nonce) {
    for (size_t i = 0; i < s_count; ++i) {
        if ((nonempty(command_id) && strcmp(command_id, s_cache[i].command_id) == 0) ||
            (nonempty(nonce) && strcmp(nonce, s_cache[i].nonce) == 0)) {
            return true;
        }
    }
    return false;
}

static void persist_cache(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char encoded[1024] = {0};
    size_t used = 0;
    for (size_t i = 0; i < s_count; ++i) {
        int n = snprintf(encoded + used, sizeof(encoded) - used, "%s%s,%s",
                         i ? ";" : "", s_cache[i].command_id, s_cache[i].nonce);
        if (n < 0 || (size_t)n >= sizeof(encoded) - used) break;
        used += (size_t)n;
    }
    nvs_set_str(h, NVS_KEY, encoded);
    nvs_commit(h);
    nvs_close(h);
}

static void load_cache(void) {
    if (s_loaded) return;
    s_loaded = true;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = 0;
    if (nvs_get_str(h, NVS_KEY, NULL, &len) != ESP_OK || len == 0 || len > 1024) {
        nvs_close(h);
        return;
    }
    char* buf = calloc(1, len);
    if (!buf) {
        nvs_close(h);
        return;
    }
    if (nvs_get_str(h, NVS_KEY, buf, &len) == ESP_OK) {
        char* save = NULL;
        for (char* token = strtok_r(buf, ";", &save);
             token && s_count < CACHE_SIZE;
             token = strtok_r(NULL, ";", &save)) {
            char* comma = strchr(token, ',');
            if (!comma) continue;
            *comma = '\0';
            snprintf(s_cache[s_count].command_id, sizeof(s_cache[s_count].command_id), "%s", token);
            snprintf(s_cache[s_count].nonce, sizeof(s_cache[s_count].nonce), "%s", comma + 1);
            s_count++;
        }
        s_next = s_count % CACHE_SIZE;
    }
    free(buf);
    nvs_close(h);
}

void command_validation_begin(void) { load_cache(); }

command_validation_result_t command_validation_validate(cJSON* doc, const char* device_mac) {
    (void)device_mac;
    load_cache();
    command_validation_result_t r = {.ok = false, .error = "invalid_command", .command_id = "", .nonce = ""};
    const char* cmd = json_string(doc, "cmd");
    const char* command_id = json_string(doc, "command_id");
    const char* issued_at = json_string(doc, "issued_at");
    const char* expires_at = json_string(doc, "expires_at");
    const char* nonce = json_string(doc, "nonce");
    const char* actor = json_string(doc, "actor");
    const char* jwt = json_string(doc, "jwt");
    const char* signature = json_string(doc, "signature");
    r.command_id = command_id;
    r.nonce = nonce;

    if (!nonempty(cmd) || !nonempty(command_id) || !nonempty(issued_at) ||
        !nonempty(expires_at) || !nonempty(nonce) || !nonempty(actor)) {
        r.error = "missing_envelope_field";
        return r;
    }
    if (!nonempty(jwt) && !nonempty(signature)) {
        r.error = "missing_authenticator";
        return r;
    }
    if (strlen(command_id) >= VALUE_MAX || strlen(nonce) >= VALUE_MAX) {
        r.error = "envelope_field_too_long";
        return r;
    }
    time_t issued = 0;
    time_t expires = 0;
    if (!parse_epoch(cJSON_GetObjectItemCaseSensitive(doc, "issued_at"), &issued) ||
        !parse_epoch(cJSON_GetObjectItemCaseSensitive(doc, "expires_at"), &expires)) {
        r.error = "invalid_timestamp";
        return r;
    }
    const bool challenge_flow = uses_server_challenge_flow(doc);
    time_t now = forgekey_time_epoch_now();
    if (expires <= issued) {
        r.error = "expired";
        return r;
    }
    if (!forgekey_time_clock_valid() && !challenge_flow) {
        r.error = "clock_invalid";
        return r;
    }
    if (forgekey_time_clock_valid() && now > expires) {
        r.error = "expired";
        return r;
    }
    if (forgekey_time_clock_valid() && issued > now + 300) {
        r.error = "issued_in_future";
        return r;
    }
    if (replay_seen(command_id, nonce)) {
        r.error = "replay_detected";
        return r;
    }
    if (!nonempty(jwt)) {
        char signing_input[384];
        int n = snprintf(signing_input, sizeof(signing_input), "%s\n%s\n%s\n%s\n%s\n%s",
                         cmd, command_id, issued_at, expires_at, nonce, actor);
        if (n < 0 || (size_t)n >= sizeof(signing_input) ||
            !lock_state_validate_command_signature(signing_input, signature)) {
            r.error = "invalid_signature";
            return r;
        }
        r.ok = true;
        r.error = NULL;
        return r;
    }
    if (!lock_state_validate_signed_command(jwt, 0, cmd)) {
        r.error = "invalid_token";
        return r;
    }
    r.ok = true;
    r.error = NULL;
    return r;
}

void command_validation_remember_accepted(const command_validation_result_t* result) {
    if (!result || !result->ok || !nonempty(result->command_id) || !nonempty(result->nonce)) return;
    snprintf(s_cache[s_next].command_id, sizeof(s_cache[s_next].command_id), "%s", result->command_id);
    snprintf(s_cache[s_next].nonce, sizeof(s_cache[s_next].nonce), "%s", result->nonce);
    if (s_count < CACHE_SIZE) s_count++;
    s_next = (s_next + 1) % CACHE_SIZE;
    persist_cache();
}
