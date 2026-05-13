/*
 * Lock state machine implementation for ESP32-C6 lock device.
 * Non-blocking state machine with GPIO debouncing, solenoid pulse,
 * and ES256 signed-command verification via mbedtls.
 */

#include "lock_state.h"
#include "lock_config.h"
#include "device_config.h"
#include "provisioning.h"
#include "oms_command_pubkey.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mbedtls/bignum.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"

static const char* TAG = "LOCK";

/* Internal state */
static lock_state_t g_state = LOCK_STATE_INITIALIZING;
static lock_trigger_t g_last_trigger = LOCK_TRIGGER_NONE;
static char g_mac_address[13] = {0};

/* GPIO pin states (debounced) */
static bool g_reed_closed = false;
static bool g_latch_locked = false;
static bool g_ir_broken = false;
static bool g_mortise_active = false;

/* Debounce tracking */
static uint32_t g_reed_debounce_time = 0;
static uint32_t g_latch_debounce_time = 0;
static uint32_t g_ir_debounce_time = 0;
static uint32_t g_mortise_debounce_time = 0;
static bool g_reed_init = false;
static bool g_latch_init = false;
static bool g_ir_init = false;
static bool g_mortise_init = false;

/* Solenoid pulse tracking */
static uint32_t g_solenoid_start_ms = 0;
static bool g_solenoid_active = false;

/* State transition tracking */
static uint32_t g_state_start_ms = 0;
static uint32_t g_last_telemetry_ms = 0;

/* Telemetry change detection */
static bool g_last_secure = false;
static bool g_last_item_present = false;

/* ===== GPIO init ===== */

void lock_state_gpio_init(void) {
    /* Solenoid - output, initially off */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << FORGEKEY_LOCK_SOLENOID_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(FORGEKEY_LOCK_SOLENOID_PIN, 0);
    ESP_LOGI(TAG, "Solenoid on GPIO %d", FORGEKEY_LOCK_SOLENOID_PIN);

    /* Reed switch - input with pullup */
    io_conf.pin_bit_mask = (1ULL << FORGEKEY_LOCK_REED_PIN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "Reed switch on GPIO %d", FORGEKEY_LOCK_REED_PIN);

    /* Mortise switch - input with pullup */
    io_conf.pin_bit_mask = (1ULL << FORGEKEY_LOCK_MORTISE_PIN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "Mortise switch on GPIO %d", FORGEKEY_LOCK_MORTISE_PIN);

    /* Latch supervisor - input with pullup */
    io_conf.pin_bit_mask = (1ULL << FORGEKEY_LOCK_LATCH_SUPERVISOR_PIN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "Latch supervisor on GPIO %d", FORGEKEY_LOCK_LATCH_SUPERVISOR_PIN);

    /* IR beam - input with pullup */
    io_conf.pin_bit_mask = (1ULL << FORGEKEY_LOCK_IR_BEAM_PIN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "IR beam on GPIO %d", FORGEKEY_LOCK_IR_BEAM_PIN);
}

void lock_state_begin(void) {
    g_state = LOCK_STATE_INITIALIZING;
    g_last_trigger = LOCK_TRIGGER_NONE;
    g_solenoid_active = false;
    g_state_start_ms = esp_timer_get_time() / 1000;
    g_last_telemetry_ms = 0;
    g_last_secure = false;
    g_last_item_present = false;
    g_reed_closed = false;
    g_latch_locked = false;
    g_ir_broken = true;
    g_mortise_active = false;
    g_reed_init = false;
    g_latch_init = false;
    g_ir_init = false;
    g_mortise_init = false;
}

/* ===== GPIO read with debouncing ===== */

static void lock_gpio_read(void) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    /* Reed switch: closed = LOW */
    bool reed_raw = (gpio_get_level(FORGEKEY_LOCK_REED_PIN) == FORGEKEY_LOCK_REED_CLOSED);
    if (!g_reed_init) {
        g_reed_closed = reed_raw;
        g_reed_debounce_time = now;
        g_reed_init = true;
    } else if (reed_raw != g_reed_closed) {
        if (now - g_reed_debounce_time >= FORGEKEY_LOCK_REED_DEBOUNCE_MS) {
            g_reed_closed = reed_raw;
            g_reed_debounce_time = now;
        }
    } else {
        g_reed_debounce_time = now;
    }

    /* Latch supervisor: locked = LOW */
    bool latch_raw = (gpio_get_level(FORGEKEY_LOCK_LATCH_SUPERVISOR_PIN) == FORGEKEY_LOCK_LATCH_LOCKED);
    if (!g_latch_init) {
        g_latch_locked = latch_raw;
        g_latch_debounce_time = now;
        g_latch_init = true;
    } else if (latch_raw != g_latch_locked) {
        if (now - g_latch_debounce_time >= FORGEKEY_LOCK_LATCH_DEBOUNCE_MS) {
            g_latch_locked = latch_raw;
            g_latch_debounce_time = now;
        }
    } else {
        g_latch_debounce_time = now;
    }

    /* IR beam: broken = HIGH (phototransistor not conducting) */
    bool ir_raw = (gpio_get_level(FORGEKEY_LOCK_IR_BEAM_PIN) == FORGEKEY_LOCK_IR_BEAM_BROKEN);
    if (!g_ir_init) {
        g_ir_broken = ir_raw;
        g_ir_debounce_time = now;
        g_ir_init = true;
    } else if (ir_raw != g_ir_broken) {
        if (now - g_ir_debounce_time >= FORGEKEY_LOCK_IR_DEBOUNCE_MS) {
            g_ir_broken = ir_raw;
            g_ir_debounce_time = now;
        }
    } else {
        g_ir_debounce_time = now;
    }

    /* Mortise switch: active = LOW */
    bool mortise_raw = (gpio_get_level(FORGEKEY_LOCK_MORTISE_PIN) == LOW);
    if (!g_mortise_init) {
        g_mortise_active = mortise_raw;
        g_mortise_debounce_time = now;
        g_mortise_init = true;
    } else if (mortise_raw != g_mortise_active) {
        if (now - g_mortise_debounce_time >= FORGEKEY_LOCK_MORTISE_DEBOUNCE_MS) {
            g_mortise_active = mortise_raw;
            g_mortise_debounce_time = now;
        }
    } else {
        g_mortise_debounce_time = now;
    }
}

/* ===== Signed-command validation ===== */

static bool base64url_decode(const char* in, size_t in_len,
                             uint8_t* out, size_t out_cap, size_t* out_len) {
    if (!in || !out || !out_len) return false;

    size_t normalized_len = in_len;
    while (normalized_len % 4 != 0) normalized_len++;

    char normalized[512];
    if (normalized_len + 1 > sizeof(normalized)) {
        return false;
    }

    for (size_t i = 0; i < in_len; i++) {
        char c = in[i];
        normalized[i] = (c == '-') ? '+' : (c == '_' ? '/' : c);
    }
    for (size_t i = in_len; i < normalized_len; i++) {
        normalized[i] = '=';
    }
    normalized[normalized_len] = '\0';

    size_t produced = 0;
    int rc = mbedtls_base64_decode(out, out_cap, &produced,
                                   (const unsigned char*)normalized,
                                   normalized_len);
    if (rc != 0) {
        return false;
    }
    *out_len = produced;
    return true;
}

static void normalize_mac_claim(const char* src, char* dest, size_t dest_len) {
    size_t j = 0;
    if (dest_len == 0) return;
    for (size_t i = 0; src && src[i] != '\0' && j + 1 < dest_len; i++) {
        char c = src[i];
        if (c == ':' || c == '-') {
            continue;
        }
        dest[j++] = (char)tolower((unsigned char)c);
    }
    dest[j] = '\0';
}

static const char* active_command_pubkey_pem(void) {
    static prov_credentials_t creds;
    creds = provisioning_credentials();
    if (creds.command_public_key_pem[0] != '\0') {
        return creds.command_public_key_pem;
    }
    return kOmsCommandPubKeyPem;
}

static bool verify_es256_signature(const char* signing_input,
                                   size_t signing_input_len,
                                   const uint8_t* signature,
                                   size_t signature_len) {
    if (!signing_input || !signature || signature_len != 64) {
        return false;
    }

    const char* pubkey_pem = active_command_pubkey_pem();
    if (!pubkey_pem || pubkey_pem[0] == '\0') {
        return false;
    }

    uint8_t digest[32];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    mbedtls_sha256_update(&sha, (const unsigned char*)signing_input, signing_input_len);
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int rc = mbedtls_pk_parse_public_key(&pk,
                                         (const unsigned char*)pubkey_pem,
                                         strlen(pubkey_pem) + 1);
    if (rc != 0) {
        ESP_LOGW(TAG, "Command pubkey parse failed: -0x%04x", -rc);
        mbedtls_pk_free(&pk);
        return false;
    }
    if (!mbedtls_pk_can_do(&pk, MBEDTLS_PK_ECKEY)) {
        ESP_LOGW(TAG, "Command pubkey is not an EC key");
        mbedtls_pk_free(&pk);
        return false;
    }

    mbedtls_mpi r;
    mbedtls_mpi s;
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    rc = mbedtls_mpi_read_binary(&r, signature, 32);
    if (rc == 0) rc = mbedtls_mpi_read_binary(&s, signature + 32, 32);
    if (rc == 0) {
        mbedtls_ecp_keypair* keypair = mbedtls_pk_ec(pk);
        rc = mbedtls_ecdsa_verify(&keypair->grp, digest, sizeof(digest),
                                  &keypair->Q, &r, &s);
    }
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_pk_free(&pk);
    return rc == 0;
}

bool lock_state_validate_signed_command(const char* token, long timestamp) {
    if (!token || token[0] == '\0') {
        ESP_LOGW(TAG, "Signed command validation: null or empty token");
        return false;
    }

    const char* first_dot = strchr(token, '.');
    const char* second_dot = first_dot ? strchr(first_dot + 1, '.') : NULL;
    if (!first_dot || !second_dot || second_dot <= first_dot + 1) {
        ESP_LOGW(TAG, "Signed command validation: malformed JWT");
        return false;
    }

    size_t payload_b64_len = (size_t)(second_dot - first_dot - 1);
    uint8_t payload_decoded[512];
    size_t payload_decoded_len = 0;
    if (!base64url_decode(first_dot + 1, payload_b64_len,
                          payload_decoded, sizeof(payload_decoded) - 1,
                          &payload_decoded_len)) {
        ESP_LOGW(TAG, "Signed command validation: payload decode failed");
        return false;
    }
    payload_decoded[payload_decoded_len] = '\0';

    cJSON* payload = cJSON_Parse((const char*)payload_decoded);
    if (!payload) {
        ESP_LOGW(TAG, "Signed command validation: payload parse failed");
        return false;
    }

    long claim_timestamp = timestamp;
    long expiry = 0;
    cJSON* field = cJSON_GetObjectItemCaseSensitive(payload, "timestamp");
    if (cJSON_IsNumber(field)) {
        claim_timestamp = (long)field->valuedouble;
    }
    field = cJSON_GetObjectItemCaseSensitive(payload, "exp");
    if (cJSON_IsNumber(field)) {
        expiry = (long)field->valuedouble;
    }

    const char* claim_mac = "";
    field = cJSON_GetObjectItemCaseSensitive(payload, "mac");
    if (cJSON_IsString(field) && field->valuestring) {
        claim_mac = field->valuestring;
    }

    const char* claim_cmd = "";
    field = cJSON_GetObjectItemCaseSensitive(payload, "cmd");
    if (cJSON_IsString(field) && field->valuestring) {
        claim_cmd = field->valuestring;
    }

    time_t now = time(NULL);
    if (claim_timestamp > 0 && now > 0) {
        long diff = labs((long)now - claim_timestamp);
        if (diff > FORGEKEY_LOCK_CMD_TIMESTAMP_TOLERANCE_S) {
            ESP_LOGW(TAG, "Signed command timestamp drift=%ld exceeds tolerance=%d",
                     diff, FORGEKEY_LOCK_CMD_TIMESTAMP_TOLERANCE_S);
            cJSON_Delete(payload);
            return false;
        }
    }
    if (expiry > 0 && now > 0 && (long)now > expiry) {
        ESP_LOGW(TAG, "Signed command expired");
        cJSON_Delete(payload);
        return false;
    }

    if (claim_mac && claim_mac[0] != '\0') {
        char expected_mac[13];
        char actual_mac[32];
        normalize_mac_claim(g_mac_address, expected_mac, sizeof(expected_mac));
        normalize_mac_claim(claim_mac, actual_mac, sizeof(actual_mac));
        if (strcmp(expected_mac, actual_mac) != 0) {
            ESP_LOGW(TAG, "Signed command MAC mismatch expected=%s got=%s",
                     expected_mac, actual_mac);
            cJSON_Delete(payload);
            return false;
        }
    }
    if (claim_cmd && claim_cmd[0] != '\0' && strcmp(claim_cmd, "unlock") != 0) {
        ESP_LOGW(TAG, "Signed command rejected unexpected cmd=%s", claim_cmd);
        cJSON_Delete(payload);
        return false;
    }

    size_t signature_len = strlen(second_dot + 1);
    uint8_t signature[80];
    size_t signature_decoded_len = 0;
    if (!base64url_decode(second_dot + 1, signature_len,
                          signature, sizeof(signature), &signature_decoded_len)) {
        ESP_LOGW(TAG, "Signed command validation: signature decode failed");
        cJSON_Delete(payload);
        return false;
    }

    bool verified = verify_es256_signature(token, (size_t)(second_dot - token),
                                           signature, signature_decoded_len);
    cJSON_Delete(payload);
    if (!verified) {
        ESP_LOGW(TAG, "Signed command validation: signature verification failed");
        return false;
    }

    return true;
}

/* ===== State machine ===== */

void lock_state_tick(void) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    /* Read sensors with debouncing */
    lock_gpio_read();

    /* Check solenoid pulse timing */
    if (g_solenoid_active && (now - g_solenoid_start_ms >= FORGEKEY_LOCK_SOLENOID_PULSE_MS)) {
        gpio_set_level(FORGEKEY_LOCK_SOLENOID_PIN, 0);
        g_solenoid_active = false;
        ESP_LOGI(TAG, "Solenoid pulse complete");
    }

    /* State machine */
    bool secure = g_reed_closed && g_latch_locked;

    switch (g_state) {
        case LOCK_STATE_INITIALIZING:
            /* Stay in initializing until MQTT connected (set by main.c) */
            break;

        case LOCK_STATE_SECURE:
            if (!secure) {
                if (!g_reed_closed) {
                    ESP_LOGW(TAG, "Door opened without unlock -> ALARM");
                    g_state = LOCK_STATE_ALARM;
                    g_state_start_ms = now;
                    g_last_trigger = LOCK_TRIGGER_ALARM_TIMEOUT;
                } else if (!g_latch_locked) {
                    ESP_LOGW(TAG, "Latch disengaged -> UNLOCKED");
                    g_state = LOCK_STATE_UNLOCKED;
                    g_state_start_ms = now;
                    g_last_trigger = LOCK_TRIGGER_SIGNED_COMMAND;
                }
            }
            break;

        case LOCK_STATE_UNLOCKED:
            /* Auto-relock timeout */
            if (FORGEKEY_LOCK_AUTO_RELOCK_MS > 0 &&
                now - g_state_start_ms >= FORGEKEY_LOCK_AUTO_RELOCK_MS) {
                g_last_trigger = LOCK_TRIGGER_AUTO_UNLOCK;
            }

            /* Door closed and latch engaged */
            if (secure) {
                ESP_LOGI(TAG, "Door closed and latch engaged -> SECURE");
                g_state = LOCK_STATE_SECURE;
                g_state_start_ms = now;
                g_last_trigger = LOCK_TRIGGER_DOOR_CLOSE;
            } else if (g_mortise_active) {
                ESP_LOGW(TAG, "Mortise switch active -> ALARM");
                g_state = LOCK_STATE_ALARM;
                g_state_start_ms = now;
                g_last_trigger = LOCK_TRIGGER_MORTISE;
            }
            break;

        case LOCK_STATE_ACCESSING:
            if (secure) {
                ESP_LOGI(TAG, "Door closed and latch engaged -> SECURE");
                g_state = LOCK_STATE_SECURE;
                g_state_start_ms = now;
                g_last_trigger = LOCK_TRIGGER_DOOR_CLOSE;
            } else if (g_mortise_active) {
                ESP_LOGW(TAG, "Mortise switch active -> ALARM");
                g_state = LOCK_STATE_ALARM;
                g_state_start_ms = now;
                g_last_trigger = LOCK_TRIGGER_MORTISE;
            }
            break;

        case LOCK_STATE_ALARM:
            if (secure) {
                ESP_LOGI(TAG, "Door closed and latch engaged -> SECURE (alarm cleared)");
                g_state = LOCK_STATE_SECURE;
                g_state_start_ms = now;
                g_last_trigger = LOCK_TRIGGER_ALARM_TIMEOUT;
            }
            break;

        default:
            ESP_LOGE(TAG, "Unknown state %d, resetting to SECURE", g_state);
            g_state = LOCK_STATE_SECURE;
            g_state_start_ms = now;
            break;
    }
}

/* ===== Public API ===== */

lock_state_t lock_state_get_state(void) {
    return g_state;
}

const char* lock_state_state_name(lock_state_t state) {
    switch (state) {
        case LOCK_STATE_INITIALIZING: return "INITIALIZING";
        case LOCK_STATE_SECURE: return "SECURE";
        case LOCK_STATE_UNLOCKED: return "UNLOCKED";
        case LOCK_STATE_ACCESSING: return "ACCESSING";
        case LOCK_STATE_ALARM: return "ALARM";
        default: return "UNKNOWN";
    }
}

lock_trigger_t lock_state_get_last_trigger(void) {
    return g_last_trigger;
}

lock_telemetry_t lock_state_get_telemetry(void) {
    lock_telemetry_t tel = {0};
    tel.secure = g_reed_closed && g_latch_locked;
    tel.reed_closed = g_reed_closed;
    tel.latch_locked = g_latch_locked;
    tel.ir_broken = g_ir_broken;
    tel.mortise_active = g_mortise_active;
    tel.uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    return tel;
}

bool lock_state_handle_unlock(const char* token, long timestamp) {
    if (!token || token[0] == '\0') {
        ESP_LOGW(TAG, "Unlock: null or empty token");
        return false;
    }

    if (!lock_state_validate_signed_command(token, timestamp)) {
        ESP_LOGW(TAG, "Unlock: signed command validation failed");
        return false;
    }

    /* Valid signed command: pulse the solenoid */
    g_solenoid_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    g_solenoid_active = true;
    gpio_set_level(FORGEKEY_LOCK_SOLENOID_PIN, FORGEKEY_LOCK_SOLENOID_ACTIVE);

    /* Transition to UNLOCKED */
    g_state = LOCK_STATE_UNLOCKED;
    g_last_trigger = LOCK_TRIGGER_SIGNED_COMMAND;

    ESP_LOGI(TAG, "Unlocked via signed command (timestamp=%ld)", timestamp);
    return true;
}

char* lock_state_get_mac_address(void) {
    return g_mac_address;
}

void lock_state_set_mac(const char* mac) {
    strncpy(g_mac_address, mac, sizeof(g_mac_address) - 1);
    g_mac_address[sizeof(g_mac_address) - 1] = '\0';
}
