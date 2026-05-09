/*
 * Credential rotation implementation for ESP32-C6 lock device.
 */

#include "credential_rotation.h"
#include "provisioning.h"

#include <string.h>
#include <stdio.h>

#include "esp_system.h"
#include "esp_log.h"

static const char* TAG = "CRED_ROT";

bool credential_rotation_handle_config(const char* topic, const uint8_t* payload, uint32_t length) {
    /* Parse JSON payload */
    char payload_copy[length + 1];
    memcpy(payload_copy, payload, length);
    payload_copy[length] = '\0';

    /* Find "provisioning_token" */
    char* tok_key = strstr(payload_copy, "\"provisioning_token\"");
    if (!tok_key) return false;

    char* tok_colon = strchr(tok_key + 20, ':');
    if (!tok_colon) return false;

    char* tok_q1 = strchr(tok_colon + 1, '"');
    if (!tok_q1) return false;

    char* tok_q2 = strchr(tok_q1 + 1, '"');
    if (!tok_q2) return false;

    size_t tok_len = tok_q2 - tok_q1 - 1;
    if (tok_len == 0 || tok_len >= 256) return false;

    char new_token[256];
    strncpy(new_token, tok_q1 + 1, tok_len);
    new_token[tok_len] = '\0';

    /* Update provisioning token */
    if (provisioning_set_token(new_token)) {
        ESP_LOGI(TAG, "Provisioning token updated via MQTT");
        return true;
    }

    ESP_LOGE(TAG, "Failed to update provisioning token");
    return false;
}

void credential_rotation_get_topic(char* topic_out, size_t topic_len, const char* mac) {
    snprintf(topic_out, topic_len, "forgekey/%s/config", mac);
}
