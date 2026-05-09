/*
 * Lock state machine implementation for ESP32-C6 lock device.
 * Non-blocking state machine with GPIO debouncing, solenoid pulse,
 * and full HMAC-SHA256 JWT verification via mbedtls.
 */

#include "lock_state.h"
#include "lock_config.h"
#include "device_config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "mbedtls/sha256.h"
#include "mbedtls/hmac_sha256.h"

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

/* ===== JWT validation with HMAC-SHA256 ===== */

/* Base64url decode helper */
static int base64url_decode(const char* in, size_t in_len, uint8_t* out, size_t* out_len) {
    static const unsigned char table[256] = {
        [65] = 0, [66] = 1, [67] = 2, [68] = 3, [69] = 4, [70] = 5,
        [71] = 6, [72] = 7, [73] = 8, [74] = 9, [75] = 10, [76] = 11,
        [77] = 12, [78] = 13, [79] = 14, [80] = 15, [81] = 16, [82] = 17,
        [83] = 18, [84] = 19, [85] = 20, [86] = 21, [87] = 22, [88] = 23,
        [89] = 24, [90] = 25,
        [97] = 26, [98] = 27, [99] = 28, [100] = 29, [101] = 30, [102] = 31,
        [103] = 32, [104] = 33, [105] = 34, [106] = 35, [107] = 36, [108] = 37,
        [109] = 38, [110] = 39, [111] = 40, [112] = 41, [113] = 42, [114] = 43,
        [115] = 44, [116] = 45, [117] = 46, [118] = 47,
        [48] = 48, [49] = 49, [50] = 50, [51] = 51, [52] = 52, [53] = 53,
        [54] = 54, [55] = 55, [56] = 56, [57] = 57,
        [43] = 62, [47] = 63,
    };

    if (!in || !out || !out_len) return -1;

    size_t decoded_len = (in_len + 3) / 4 * 3;
    uint8_t tmp[4];
    int tmp_pos = 0;
    *out_len = 0;

    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '-') tmp[4] = 62; /* base64url uses - and _ instead of + and / */
        else if (c == '_') tmp[4] = 63;
        else if (c == '=') tmp[4] = 0;
        else if (c >= 'A' && c <= 'Z') tmp[4] = table[c];
        else if (c >= 'a' && c <= 'z') tmp[4] = table[c];
        else if (c >= '0' && c <= '9') tmp[4] = table[c] - 26;
        else continue; /* skip padding and invalid chars */

        tmp[tmp_pos++] = tmp[4];

        if (tmp_pos == 4) {
            out[(*out_len)++] = (tmp[0] << 2) | (tmp[1] >> 4);
            if ((*out_len) < decoded_len) {
                out[(*out_len)++] = (tmp[1] << 4) | (tmp[2] >> 2);
            }
            if ((*out_len) < decoded_len) {
                out[(*out_len)++] = (tmp[2] << 6) | tmp[3];
            }
            tmp_pos = 0;
        }
    }

    return 0;
}

/* Find a character in a string, stopping at a specific character */
static const char* strnstr_limit(const char* haystack, char needle, const char* limit) {
    while (haystack < limit && *haystack != needle) {
        haystack++;
    }
    return (haystack < limit) ? haystack : NULL;
}

bool lock_state_validate_jwt(const char* token, long timestamp) {
    if (!token || token[0] == '\0') {
        ESP_LOGW(TAG, "JWT validation: null or empty token");
        return false;
    }

    /* Parse JWT: header.payload.signature */
    int first_dot = -1, second_dot = -1;
    size_t token_len = strlen(token);

    for (size_t i = 0; i < token_len; i++) {
        if (token[i] == '.' && first_dot < 0) {
            first_dot = (int)i;
        } else if (token[i] == '.' && first_dot >= 0 && second_dot < 0) {
            second_dot = (int)i;
            break;
        }
    }

    if (first_dot <= 0 || second_dot <= first_dot + 1) {
        ESP_LOGW(TAG, "JWT validation: malformed JWT (bad dot positions)");
        return false;
    }

    /* Extract payload */
    size_t payload_len = second_dot - first_dot - 1;
    uint8_t payload_decoded[256];
    size_t payload_decoded_len = 0;

    if (base64url_decode(token + first_dot + 1, payload_len,
                         payload_decoded, &payload_decoded_len) != 0) {
        ESP_LOGW(TAG, "JWT validation: base64url decode failed");
        return false;
    }

    /* Parse JSON to find "exp" and "timestamp" fields */
    long jwt_timestamp = 0;
    long jwt_expiry = 0;

    /* Find "timestamp" */
    char* ts_key = (char*)memmem(payload_decoded, payload_decoded_len, "\"timestamp\"", 11);
    if (ts_key) {
        char* colon = strchr(ts_key + 11, ':');
        if (colon) {
            char* num_start = colon + 1;
            while (*num_start == ' ' || *num_start == '"') num_start++;
            jwt_timestamp = atol(num_start);
        }
    }

    /* Find "exp" */
    char* exp_key = (char*)memmem(payload_decoded, payload_decoded_len, "\"exp\"", 5);
    if (exp_key) {
        char* colon = strchr(exp_key + 5, ':');
        if (colon) {
            char* num_start = colon + 1;
            while (*num_start == ' ' || *num_start == '"') num_start++;
            jwt_expiry = atol(num_start);
        }
    }

    /* Check timestamp tolerance */
    time_t now = time(NULL);
    if (now > 0) {
        long diff = labs(now - jwt_timestamp);
        if (diff > FORGEKEY_LOCK_CMD_TIMESTAMP_TOLERANCE_S) {
            ESP_LOGW(TAG, "JWT validation: timestamp tolerance failed (diff=%ld > %d)",
                     diff, FORGEKEY_LOCK_CMD_TIMESTAMP_TOLERANCE_S);
            return false;
        }
    }

    /* Check JWT expiry */
    if (jwt_expiry > 0 && now > 0) {
        if ((long)now > jwt_expiry) {
            ESP_LOGW(TAG, "JWT validation: token expired (exp=%ld, now=%ld)", jwt_expiry, (long)now);
            return false;
        }
    }

    /* HMAC-SHA256 signature verification */
    size_t sig_len = token_len - second_dot - 1;
    uint8_t signature[128];
    size_t signature_decoded_len = 0;

    if (base64url_decode(token + second_dot + 1, sig_len,
                         signature, &signature_decoded_len) != 0) {
        ESP_LOGW(TAG, "JWT validation: signature base64url decode failed");
        return false;
    }

    /* Compute expected HMAC-SHA256 of header.payload */
    uint8_t hmac_result[32];
    mbedtls_hmac_sha256((const unsigned char*)FORGEKEY_LOCK_JWT_SECRET,
                        strlen(FORGEKEY_LOCK_JWT_SECRET),
                        (const unsigned char*)token, second_dot,
                        hmac_result, 32);

    /* Compare signatures */
    if (signature_decoded_len != 32) {
        ESP_LOGW(TAG, "JWT validation: signature length mismatch (%zu != 32)", signature_decoded_len);
        return false;
    }

    if (memcmp(hmac_result, signature, 32) != 0) {
        ESP_LOGW(TAG, "JWT validation: HMAC-SHA256 signature mismatch");
        return false;
    }

    ESP_LOGI(TAG, "JWT validation: OK (timestamp=%ld, exp=%ld)", jwt_timestamp, jwt_expiry);
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
                    g_last_trigger = LOCK_TRIGGER_JWT;
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

    if (!lock_state_validate_jwt(token, timestamp)) {
        ESP_LOGW(TAG, "Unlock: JWT validation failed");
        return false;
    }

    /* Valid JWT: pulse the solenoid */
    g_solenoid_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    g_solenoid_active = true;
    gpio_set_level(FORGEKEY_LOCK_SOLENOID_PIN, FORGEKEY_LOCK_SOLENOID_ACTIVE);

    /* Transition to UNLOCKED */
    g_state = LOCK_STATE_UNLOCKED;
    g_last_trigger = LOCK_TRIGGER_JWT;

    ESP_LOGI(TAG, "Unlocked via JWT (timestamp=%ld)", timestamp);
    return true;
}

char* lock_state_get_mac_address(void) {
    return g_mac_address;
}

void lock_state_set_mac(const char* mac) {
    strncpy(g_mac_address, mac, sizeof(g_mac_address) - 1);
    g_mac_address[sizeof(g_mac_address) - 1] = '\0';
}
