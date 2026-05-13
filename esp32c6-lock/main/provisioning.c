/*
 * Provisioning implementation for ESP32-C6 lock device.
 * NVS-backed credential storage + OMS registration via esp_http_client.
 */

#include "provisioning.h"
#include "device_config.h"
#include "oms_ca.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

static const char* TAG = "PROV";

#define NVS_NAMESPACE "forgekey"
#define NVS_KEY_DEV_ID    "dev_id"
#define NVS_KEY_FW_TOPIC  "fw_topic"
#define NVS_KEY_P_TOPIC   "p_topic"
#define NVS_KEY_JWT       "jwt"
#define NVS_KEY_B_HOST    "b_host"
#define NVS_KEY_B_PORT    "b_port"
#define NVS_KEY_B_TLS     "b_tls"
#define NVS_KEY_BOOTS     "boots"
#define NVS_KEY_PROV_TOK  "prov_tok"
#define NVS_KEY_ASSET_ID  "asset_id"

#define BOUNDARY "----ForgekeyBoundary7d3f2c1a"

static prov_credentials_t s_creds;
static uint32_t s_boot_count = 0;

static const char* chip_model_name(esp_chip_model_t model) {
    switch (model) {
        case CHIP_ESP32:
            return "ESP32";
        case CHIP_ESP32S2:
            return "ESP32-S2";
        case CHIP_ESP32S3:
            return "ESP32-S3";
        case CHIP_ESP32C3:
            return "ESP32-C3";
        case CHIP_ESP32H2:
            return "ESP32-H2";
        default:
            return "Unknown";
    }
}

static void add_chip_features_json(cJSON* features, uint32_t chip_features) {
    cJSON_AddBoolToObject(features, "embedded_flash",
                          (chip_features & CHIP_FEATURE_EMB_FLASH) != 0);
    cJSON_AddBoolToObject(features, "wifi_bgn",
                          (chip_features & CHIP_FEATURE_WIFI_BGN) != 0);
    cJSON_AddBoolToObject(features, "ble",
                          (chip_features & CHIP_FEATURE_BLE) != 0);
    cJSON_AddBoolToObject(features, "bt",
                          (chip_features & CHIP_FEATURE_BT) != 0);
    cJSON_AddBoolToObject(features, "ieee802154",
                          (chip_features & CHIP_FEATURE_IEEE802154) != 0);
    cJSON_AddBoolToObject(features, "embedded_psram",
                          (chip_features & CHIP_FEATURE_EMB_PSRAM) != 0);
}

static char* build_registration_metadata_json(const char* mac, const char* ip_addr) {
    cJSON* meta = cJSON_CreateObject();
    if (!meta) {
        return NULL;
    }

    esp_chip_info_t chip_info = {0};
    esp_chip_info(&chip_info);

    cJSON_AddStringToObject(meta, "mac_address", mac);
    cJSON_AddStringToObject(meta, "firmware_version", FORGEKEY_FIRMWARE_VERSION);
    cJSON_AddStringToObject(meta, "sensor_kind", FORGEKEY_SENSOR_KIND);
    cJSON_AddNumberToObject(meta, "boot_count", (double)s_boot_count);
    cJSON_AddNumberToObject(meta, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddStringToObject(meta, "ip", ip_addr);

    cJSON* chip_info_json = cJSON_AddObjectToObject(meta, "chip_info");
    if (chip_info_json) {
        cJSON_AddStringToObject(chip_info_json, "model", chip_model_name(chip_info.model));
        cJSON_AddNumberToObject(chip_info_json, "cores", chip_info.cores);
        cJSON_AddNumberToObject(chip_info_json, "revision", chip_info.revision);
        cJSON_AddNumberToObject(chip_info_json, "full_revision", chip_info.full_revision);

        cJSON* features = cJSON_AddObjectToObject(chip_info_json, "features");
        if (features) {
            add_chip_features_json(features, chip_info.features);
        }
    }

    uint8_t base_mac[6];
    if (esp_efuse_mac_get_default(base_mac) == ESP_OK) {
        uint64_t unique_chip_id = 0;
        for (size_t i = 0; i < sizeof(base_mac); i++) {
            unique_chip_id = (unique_chip_id << 8) | base_mac[i];
        }
        char unique_chip_id_hex[17];
        snprintf(unique_chip_id_hex, sizeof(unique_chip_id_hex), "%016llx",
                 (unsigned long long)unique_chip_id);
        cJSON_AddStringToObject(meta, "unique_chip_id", unique_chip_id_hex);
    } else {
        cJSON_AddNullToObject(meta, "unique_chip_id");
        ESP_LOGW(TAG, "Failed to read eFuse MAC for unique chip id");
    }

    uint32_t flash_memory_id = 0;
    esp_err_t flash_id_err = esp_flash_read_id(NULL, &flash_memory_id);
    if (flash_id_err == ESP_OK) {
        char flash_memory_id_hex[11];
        snprintf(flash_memory_id_hex, sizeof(flash_memory_id_hex), "0x%06lx",
                 (unsigned long)flash_memory_id);
        cJSON_AddStringToObject(meta, "flash_memory_id", flash_memory_id_hex);
    } else {
        cJSON_AddNullToObject(meta, "flash_memory_id");
        ESP_LOGW(TAG, "Failed to read flash memory id: %s", esp_err_to_name(flash_id_err));
    }

    char* meta_json = cJSON_PrintUnformatted(meta);
    cJSON_Delete(meta);
    return meta_json;
}

/* Multipart body builder */
typedef struct {
    char* buf;
    size_t len;
    size_t cap;
} multipart_buf_t;

static void mb_init(multipart_buf_t* mb) {
    mb->cap = 2048;
    mb->buf = malloc(mb->cap);
    mb->len = 0;
}

static void mb_free(multipart_buf_t* mb) {
    free(mb->buf);
    mb->buf = NULL;
    mb->len = 0;
    mb->cap = 0;
}

static void mb_append(multipart_buf_t* mb, const char* data, size_t len) {
    if (mb->len + len + 64 > mb->cap) {
        mb->cap = (mb->len + len + 64) * 2;
        char* new_buf = realloc(mb->buf, mb->cap);
        if (!new_buf) return;
        mb->buf = new_buf;
    }
    memcpy(mb->buf + mb->len, data, len);
    mb->len += len;
}

static void mb_append_str(multipart_buf_t* mb, const char* str) {
    mb_append(mb, str, strlen(str));
}

static void mb_append_hex(multipart_buf_t* mb, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", data[i]);
        mb_append(mb, hex, 2);
    }
}

/* URL-encode a string for form data */
static size_t url_encode(const char* src, char* dst, size_t dst_len) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_len - 1; i++) {
        char c = src[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[j++] = c;
        } else {
            if (j + 3 >= dst_len) break;
            snprintf(dst + j, 4, "%%%02X", (unsigned char)c);
            j += 3;
        }
    }
    dst[j] = '\0';
    return j;
}

void provisioning_begin(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        s_boot_count = 0;
        return;
    }

    uint32_t boots = 0;
    nvs_get_u32(nvs_handle, NVS_KEY_BOOTS, &boots);
    boots++;
    nvs_set_u32(nvs_handle, NVS_KEY_BOOTS, boots);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    s_boot_count = boots;
    ESP_LOGI(TAG, "Boot count: %lu", (unsigned long)boots);

    /* Load credentials */
    size_t dev_id_len = sizeof(s_creds.device_id);
    size_t fw_topic_len = sizeof(s_creds.mqtt_firmware_topic);
    size_t p_topic_len = sizeof(s_creds.mqtt_pings_topic);
    size_t jwt_len = sizeof(s_creds.jwt_token);
    size_t b_host_len = sizeof(s_creds.mqtt_broker_host);
    uint16_t b_port = 0;
    uint8_t b_tls = 0;

    if (nvs_get_str(nvs_handle, NVS_KEY_DEV_ID, s_creds.device_id, &dev_id_len) != ESP_OK) {
        s_creds.device_id[0] = '\0';
    }
    if (nvs_get_str(nvs_handle, NVS_KEY_FW_TOPIC, s_creds.mqtt_firmware_topic, &fw_topic_len) != ESP_OK) {
        s_creds.mqtt_firmware_topic[0] = '\0';
    }
    if (nvs_get_str(nvs_handle, NVS_KEY_P_TOPIC, s_creds.mqtt_pings_topic, &p_topic_len) != ESP_OK) {
        s_creds.mqtt_pings_topic[0] = '\0';
    }
    if (nvs_get_str(nvs_handle, NVS_KEY_JWT, s_creds.jwt_token, &jwt_len) != ESP_OK) {
        s_creds.jwt_token[0] = '\0';
    }
    if (nvs_get_str(nvs_handle, NVS_KEY_B_HOST, s_creds.mqtt_broker_host, &b_host_len) != ESP_OK) {
        s_creds.mqtt_broker_host[0] = '\0';
    }
    nvs_get_u16(nvs_handle, NVS_KEY_B_PORT, &b_port);
    s_creds.mqtt_broker_port = b_port;
    nvs_get_u8(nvs_handle, NVS_KEY_B_TLS, &b_tls);
    s_creds.mqtt_broker_use_tls = (b_tls != 0);

    /* Load asset_id */
    size_t asset_id_len = sizeof(s_creds.asset_id);
    if (nvs_get_str(nvs_handle, NVS_KEY_ASSET_ID, s_creds.asset_id, &asset_id_len) != ESP_OK) {
        s_creds.asset_id[0] = '\0';
    }

    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "Loaded: dev_id='%s' jwt_len=%u",
             s_creds.device_id, (unsigned)s_creds.jwt_token[0] ? strlen(s_creds.jwt_token) : 0);
}

bool provisioning_is_provisioned(void) {
    return s_creds.device_id[0] != '\0' && s_creds.jwt_token[0] != '\0';
}

prov_credentials_t provisioning_credentials(void) {
    return s_creds;
}

bool provisioning_persist(const prov_credentials_t* creds) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for persist");
        return false;
    }

    nvs_set_str(nvs_handle, NVS_KEY_DEV_ID, creds->device_id);
    nvs_set_str(nvs_handle, NVS_KEY_FW_TOPIC, creds->mqtt_firmware_topic);
    nvs_set_str(nvs_handle, NVS_KEY_P_TOPIC, creds->mqtt_pings_topic);
    nvs_set_str(nvs_handle, NVS_KEY_JWT, creds->jwt_token);
    nvs_set_str(nvs_handle, NVS_KEY_B_HOST, creds->mqtt_broker_host);
    nvs_set_u16(nvs_handle, NVS_KEY_B_PORT, creds->mqtt_broker_port);
    nvs_set_u8(nvs_handle, NVS_KEY_B_TLS, creds->mqtt_broker_use_tls ? 1 : 0);
    if (creds->asset_id[0] != '\0') {
        nvs_set_str(nvs_handle, NVS_KEY_ASSET_ID, creds->asset_id);
    }
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    s_creds = *creds;
    ESP_LOGI(TAG, "Credentials persisted");
    return true;
}

void provisioning_clear(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return;

    nvs_erase_key(nvs_handle, NVS_KEY_DEV_ID);
    nvs_erase_key(nvs_handle, NVS_KEY_FW_TOPIC);
    nvs_erase_key(nvs_handle, NVS_KEY_P_TOPIC);
    nvs_erase_key(nvs_handle, NVS_KEY_JWT);
    nvs_erase_key(nvs_handle, NVS_KEY_B_HOST);
    nvs_erase_key(nvs_handle, NVS_KEY_B_PORT);
    nvs_erase_key(nvs_handle, NVS_KEY_B_TLS);
    nvs_erase_key(nvs_handle, NVS_KEY_ASSET_ID);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    memset(&s_creds, 0, sizeof(s_creds));
    ESP_LOGI(TAG, "Credentials cleared");
}

const char* provisioning_active_token(void) {
    static char stored_token[256];
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return FORGEKEY_PROVISIONING_TOKEN;
    }

    size_t tok_len = sizeof(stored_token);
    if (nvs_get_str(nvs_handle, NVS_KEY_PROV_TOK, stored_token, &tok_len) == ESP_OK) {
        nvs_close(nvs_handle);
        if (stored_token[0] != '\0') {
            return stored_token;
        }
    }
    nvs_close(nvs_handle);
    return FORGEKEY_PROVISIONING_TOKEN;
}

bool provisioning_set_token(const char* token) {
    if (!token || token[0] == '\0') return false;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return false;

    nvs_set_str(nvs_handle, NVS_KEY_PROV_TOK, token);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "Provisioning token updated in NVS");
    return true;
}

bool provisioning_register(const char* host, uint16_t port,
                           const char* mac, const char* ip_addr,
                           const uint8_t* jpeg_buf, size_t jpeg_len) {
    bool has_photo = (jpeg_buf != NULL && jpeg_len > 0);

    char* meta_json = build_registration_metadata_json(mac, ip_addr);
    if (!meta_json) {
        ESP_LOGE(TAG, "Failed to build registration metadata JSON");
        return false;
    }

    /* Build multipart body */
    multipart_buf_t mb;
    mb_init(&mb);

    /* metadata part header */
    char meta_header[256];
    int meta_hdr_len = snprintf(meta_header, sizeof(meta_header),
        "--%s\r\nContent-Disposition: form-data; name=\"metadata\"\r\n"
        "Content-Type: application/json\r\n\r\n", BOUNDARY);
    mb_append(&mb, meta_header, meta_hdr_len);

    /* metadata part body */
    mb_append_str(&mb, meta_json);
    free(meta_json);

    /* photo part header */
    if (has_photo) {
        char photo_header[256];
        int photo_hdr_len = snprintf(photo_header, sizeof(photo_header),
            "\r\n--%s\r\nContent-Disposition: form-data; name=\"photo\"; filename=\"boot.jpg\"\r\n"
            "Content-Type: image/jpeg\r\n\r\n", BOUNDARY);
        mb_append(&mb, photo_header, photo_hdr_len);
    }

    /* tail */
    const char* tail = "\r\n--" BOUNDARY "--\r\n";
    mb_append(&mb, tail, strlen(tail));

    size_t total_len = mb.len + (has_photo ? jpeg_len : 0);
    const char* active_token = provisioning_active_token();

    ESP_LOGI(TAG, "Registering: host=%s port=%u mac=%s body=%u",
             host, port, mac, (unsigned)total_len);
    if (strcmp(active_token, "REPLACE_ME_PROVISIONING_TOKEN") == 0) {
        ESP_LOGW(TAG, "WARNING: using placeholder provisioning token");
    }

    /* Build URL */
    char url[256];
    snprintf(url, sizeof(url), "https://%s:%u/api/forgekey/devices/register/", host, port);

    /* HTTP client config */
    esp_http_client_config_t http_cfg = {
        .url = url,
        .cert_pem = kOmsCaPem,
        .timeout_ms = 15000,
        .disable_auto_redirect = false,
    };

    esp_http_client_handle_t http_client = esp_http_client_init(&http_cfg);

    esp_http_client_set_method(http_client, HTTP_METHOD_POST);
    esp_http_client_set_header(http_client, "Content-Type",
                               "multipart/form-data; boundary=" BOUNDARY);
    char cl_buf[16];
    snprintf(cl_buf, sizeof(cl_buf), "%u", (unsigned)total_len);
    esp_http_client_set_header(http_client, "Content-Length", cl_buf);
    esp_http_client_set_header(http_client, "X-ForgeKey-Provisioning-Token", active_token);

    esp_err_t err = esp_http_client_open(http_client, (int)total_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(http_client);
        mb_free(&mb);
        return false;
    }

    /* Write body */
    int written = esp_http_client_write(http_client, mb.buf, (int)mb.len);
    if (written < 0) {
        ESP_LOGE(TAG, "HTTP write failed for metadata");
        esp_http_client_close(http_client);
        esp_http_client_cleanup(http_client);
        mb_free(&mb);
        return false;
    }

    if (has_photo) {
        int remaining = (int)jpeg_len;
        const uint8_t* ptr = jpeg_buf;
        while (remaining > 0) {
            int chunk = remaining > 1024 ? 1024 : remaining;
            written = esp_http_client_write(http_client, (const char*)ptr, chunk);
            if (written <= 0) {
                ESP_LOGE(TAG, "HTTP write failed for photo");
                esp_http_client_close(http_client);
                esp_http_client_cleanup(http_client);
                mb_free(&mb);
                return false;
            }
            ptr += written;
            remaining -= written;
        }
    }

    esp_http_client_close(http_client);

    int status_code = esp_http_client_get_status_code(http_client);
    ESP_LOGI(TAG, "Registration HTTP %d", status_code);

    /* Read response body */
    char resp_buf[1024];
    int body_len = esp_http_client_read(http_client, resp_buf, sizeof(resp_buf) - 1);
    esp_http_client_cleanup(http_client);
    mb_free(&mb);

    if (body_len < 0) {
        ESP_LOGE(TAG, "HTTP read failed");
        return false;
    }
    resp_buf[body_len] = '\0';

    if (status_code < 200 || status_code >= 300) {
        ESP_LOGE(TAG, "Registration failed: HTTP %d", status_code);
        return false;
    }

    /* Parse JSON response */
    prov_credentials_t new_creds;
    memset(&new_creds, 0, sizeof(new_creds));

    /* Simple JSON field extraction */
    char* p = resp_buf;
    char* key = strstr(p, "\"device_id\"");
    if (key) {
        char* colon = strchr(key + 11, ':');
        if (colon) {
            char* q1 = strchr(colon + 1, '"');
            if (q1) {
                char* q2 = strchr(q1 + 1, '"');
                if (q2) {
                    size_t len = q2 - q1 - 1;
                    if (len < sizeof(new_creds.device_id)) {
                        memcpy(new_creds.device_id, q1 + 1, len);
                        new_creds.device_id[len] = '\0';
                    }
                }
            }
        }
    }

    key = strstr(p, "\"mqtt_topic_for_firmware\"");
    if (key) {
        char* colon = strchr(key + 23, ':');
        if (colon) {
            char* q1 = strchr(colon + 1, '"');
            if (q1) {
                char* q2 = strchr(q1 + 1, '"');
                if (q2) {
                    size_t len = q2 - q1 - 1;
                    if (len < sizeof(new_creds.mqtt_firmware_topic)) {
                        memcpy(new_creds.mqtt_firmware_topic, q1 + 1, len);
                        new_creds.mqtt_firmware_topic[len] = '\0';
                    }
                }
            }
        }
    }

    key = strstr(p, "\"mqtt_topic_for_pings\"");
    if (key) {
        char* colon = strchr(key + 20, ':');
        if (colon) {
            char* q1 = strchr(colon + 1, '"');
            if (q1) {
                char* q2 = strchr(q1 + 1, '"');
                if (q2) {
                    size_t len = q2 - q1 - 1;
                    if (len < sizeof(new_creds.mqtt_pings_topic)) {
                        memcpy(new_creds.mqtt_pings_topic, q1 + 1, len);
                        new_creds.mqtt_pings_topic[len] = '\0';
                    }
                }
            }
        }
    }

    key = strstr(p, "\"jwt_token\"");
    if (key) {
        char* colon = strchr(key + 11, ':');
        if (colon) {
            char* q1 = strchr(colon + 1, '"');
            if (q1) {
                char* q2 = strchr(q1 + 1, '"');
                if (q2) {
                    size_t len = q2 - q1 - 1;
                    if (len < sizeof(new_creds.jwt_token)) {
                        memcpy(new_creds.jwt_token, q1 + 1, len);
                        new_creds.jwt_token[len] = '\0';
                    }
                }
            }
        }
    }

    key = strstr(p, "\"mqtt_broker_host\"");
    if (key) {
        char* colon = strchr(key + 18, ':');
        if (colon) {
            char* q1 = strchr(colon + 1, '"');
            if (q1) {
                char* q2 = strchr(q1 + 1, '"');
                if (q2) {
                    size_t len = q2 - q1 - 1;
                    if (len < sizeof(new_creds.mqtt_broker_host)) {
                        memcpy(new_creds.mqtt_broker_host, q1 + 1, len);
                        new_creds.mqtt_broker_host[len] = '\0';
                    }
                }
            }
        }
    }

    key = strstr(p, "\"mqtt_broker_port\"");
    if (key) {
        char* colon = strchr(key + 18, ':');
        if (colon) {
            char* num_start = colon + 1;
            while (*num_start == ' ' || *num_start == '"') num_start++;
            new_creds.mqtt_broker_port = (uint16_t)atoi(num_start);
        }
    }

    key = strstr(p, "\"mqtt_broker_use_tls\"");
    if (key) {
        char* colon = strchr(key + 19, ':');
        if (colon) {
            char* val = colon + 1;
            while (*val == ' ' || *val == '"') val++;
            new_creds.mqtt_broker_use_tls = (*val == 't' || *val == '1');
        }
    }

    /* Parse asset_id (optional) */
    key = strstr(p, "\"asset_id\"");
    if (key) {
        char* colon = strchr(key + 11, ':');
        if (colon) {
            char* q1 = strchr(colon + 1, '"');
            if (q1) {
                char* q2 = strchr(q1 + 1, '"');
                if (q2) {
                    size_t len = q2 - q1 - 1;
                    if (len < sizeof(new_creds.asset_id)) {
                        memcpy(new_creds.asset_id, q1 + 1, len);
                        new_creds.asset_id[len] = '\0';
                    }
                }
            }
        }
    }

    if (new_creds.device_id[0] == '\0' || new_creds.jwt_token[0] == '\0') {
        ESP_LOGE(TAG, "Registration response missing device_id or jwt_token");
        return false;
    }

    ESP_LOGI(TAG, "Registered as %s", new_creds.device_id);

    if (!provisioning_persist(&new_creds)) {
        ESP_LOGE(TAG, "Failed to persist credentials");
        return false;
    }

    return true;
}

uint32_t provisioning_boot_count(void) {
    return s_boot_count;
}

const char* provisioning_get_asset_id(void) {
    return s_creds.asset_id[0] != '\0' ? s_creds.asset_id : "";
}
