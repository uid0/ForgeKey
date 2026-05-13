/*
 * Provisioning implementation for ESP32-C6 lock device.
 * NVS-backed credential storage + OMS enrollment via esp_http_client.
 */

#include "provisioning.h"
#include "device_config.h"
#include "oms_ca.h"
#include "oms_command_pubkey.h"

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
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_csr.h"

static const char* TAG = "PROV";

#define NVS_NAMESPACE "forgekey"
#define NVS_KEY_DEV_ID    "dev_id"
#define NVS_KEY_FW_TOPIC  "fw_topic"
#define NVS_KEY_P_TOPIC   "p_topic"
#define NVS_KEY_CERT      "c_cert"
#define NVS_KEY_KEY       "c_key"
#define NVS_KEY_CMD_PUB   "cmd_pub"
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

static bool generate_device_key_and_csr(const char* mac,
                                        char* private_key_pem,
                                        size_t private_key_len,
                                        char* csr_pem,
                                        size_t csr_len) {
    const char* personalization = "forgekey-enroll";
    char subject_name[64];
    snprintf(subject_name, sizeof(subject_name), "CN=forgekey-%s", mac);

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    mbedtls_entropy_context entropy;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_x509write_csr req;
    mbedtls_x509write_csr_init(&req);

    int rc = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                   (const unsigned char*)personalization,
                                   strlen(personalization));
    if (rc != 0) {
        ESP_LOGE(TAG, "ctr_drbg_seed failed: -0x%04x", -rc);
        goto fail;
    }

    rc = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (rc != 0) {
        ESP_LOGE(TAG, "pk_setup failed: -0x%04x", -rc);
        goto fail;
    }

    rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk),
                             mbedtls_ctr_drbg_random, &ctr_drbg);
    if (rc != 0) {
        ESP_LOGE(TAG, "ecp_gen_key failed: -0x%04x", -rc);
        goto fail;
    }

    rc = mbedtls_pk_write_key_pem(&pk, (unsigned char*)private_key_pem, private_key_len);
    if (rc != 0) {
        ESP_LOGE(TAG, "write_key_pem failed: -0x%04x", -rc);
        goto fail;
    }

    mbedtls_x509write_csr_set_md_alg(&req, MBEDTLS_MD_SHA256);
    mbedtls_x509write_csr_set_key(&req, &pk);
    rc = mbedtls_x509write_csr_set_subject_name(&req, subject_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "set_subject_name failed: -0x%04x", -rc);
        goto fail;
    }

    rc = mbedtls_x509write_csr_pem(&req, (unsigned char*)csr_pem, csr_len,
                                   mbedtls_ctr_drbg_random, &ctr_drbg);
    if (rc != 0) {
        ESP_LOGE(TAG, "write_csr_pem failed: -0x%04x", -rc);
        goto fail;
    }

    mbedtls_x509write_csr_free(&req);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    mbedtls_pk_free(&pk);
    return true;

fail:
    mbedtls_x509write_csr_free(&req);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    mbedtls_pk_free(&pk);
    return false;
}

static char* build_enrollment_metadata_json(const char* mac,
                                            const char* ip_addr,
                                            const char* csr_pem) {
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
    cJSON_AddStringToObject(meta, "csr_pem", csr_pem);

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

    s_boot_count = boots;
    ESP_LOGI(TAG, "Boot count: %lu", (unsigned long)boots);

    /* Load credentials */
    size_t dev_id_len = sizeof(s_creds.device_id);
    size_t fw_topic_len = sizeof(s_creds.mqtt_firmware_topic);
    size_t p_topic_len = sizeof(s_creds.mqtt_pings_topic);
    size_t cert_len = sizeof(s_creds.client_certificate_pem);
    size_t key_len = sizeof(s_creds.client_private_key_pem);
    size_t cmd_pub_len = sizeof(s_creds.command_public_key_pem);
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
    if (nvs_get_str(nvs_handle, NVS_KEY_CERT, s_creds.client_certificate_pem, &cert_len) != ESP_OK) {
        s_creds.client_certificate_pem[0] = '\0';
    }
    if (nvs_get_str(nvs_handle, NVS_KEY_KEY, s_creds.client_private_key_pem, &key_len) != ESP_OK) {
        s_creds.client_private_key_pem[0] = '\0';
    }
    if (nvs_get_str(nvs_handle, NVS_KEY_CMD_PUB, s_creds.command_public_key_pem, &cmd_pub_len) != ESP_OK) {
        s_creds.command_public_key_pem[0] = '\0';
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

    if (s_creds.command_public_key_pem[0] == '\0') {
        strncpy(s_creds.command_public_key_pem, kOmsCommandPubKeyPem,
                sizeof(s_creds.command_public_key_pem) - 1);
        s_creds.command_public_key_pem[sizeof(s_creds.command_public_key_pem) - 1] = '\0';
    }

    ESP_LOGI(TAG, "Loaded: dev_id='%s' cert_len=%u key_len=%u cmd_pub_len=%u",
             s_creds.device_id,
             (unsigned)strlen(s_creds.client_certificate_pem),
             (unsigned)strlen(s_creds.client_private_key_pem),
             (unsigned)strlen(s_creds.command_public_key_pem));
}

bool provisioning_is_provisioned(void) {
    return s_creds.device_id[0] != '\0' &&
           s_creds.client_certificate_pem[0] != '\0' &&
           s_creds.client_private_key_pem[0] != '\0';
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
    nvs_set_str(nvs_handle, NVS_KEY_CERT, creds->client_certificate_pem);
    nvs_set_str(nvs_handle, NVS_KEY_KEY, creds->client_private_key_pem);
    nvs_set_str(nvs_handle, NVS_KEY_CMD_PUB, creds->command_public_key_pem);
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
    nvs_erase_key(nvs_handle, NVS_KEY_CERT);
    nvs_erase_key(nvs_handle, NVS_KEY_KEY);
    nvs_erase_key(nvs_handle, NVS_KEY_CMD_PUB);
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

bool provisioning_enroll(const char* host, uint16_t port,
                         const char* mac, const char* ip_addr,
                         const uint8_t* jpeg_buf, size_t jpeg_len) {
    bool has_photo = (jpeg_buf != NULL && jpeg_len > 0);
    char private_key_pem[FORGEKEY_PROV_MAX_KEY];
    char csr_pem[FORGEKEY_PROV_MAX_CERT];

    if (!generate_device_key_and_csr(mac, private_key_pem, sizeof(private_key_pem),
                                     csr_pem, sizeof(csr_pem))) {
        ESP_LOGE(TAG, "Failed to generate device keypair/CSR");
        return false;
    }

    char* meta_json = build_enrollment_metadata_json(mac, ip_addr, csr_pem);
    if (!meta_json) {
        ESP_LOGE(TAG, "Failed to build enrollment metadata JSON");
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

    const char* tail = "\r\n--" BOUNDARY "--\r\n";
    size_t total_len = mb.len + (has_photo ? jpeg_len : 0) + strlen(tail);
    const char* active_token = provisioning_active_token();

    ESP_LOGI(TAG, "Enrolling: host=%s port=%u mac=%s body=%u csr_len=%u",
             host, port, mac, (unsigned)total_len, (unsigned)strlen(csr_pem));
    if (strcmp(active_token, "REPLACE_ME_PROVISIONING_TOKEN") == 0) {
        ESP_LOGW(TAG, "WARNING: using placeholder provisioning token");
    }

    /* Build URL */
    char url[256];
    snprintf(url, sizeof(url), "https://%s:%u/api/forgekey/devices/enroll/", host, port);

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

    written = esp_http_client_write(http_client, tail, (int)strlen(tail));
    if (written < 0) {
        ESP_LOGE(TAG, "HTTP write failed for multipart tail");
        esp_http_client_close(http_client);
        esp_http_client_cleanup(http_client);
        mb_free(&mb);
        return false;
    }

    int fetch_len = esp_http_client_fetch_headers(http_client);
    int status_code = esp_http_client_get_status_code(http_client);
    ESP_LOGI(TAG, "Enrollment HTTP %d content_len=%d", status_code, fetch_len);

    /* Read response body */
    char resp_buf[8192];
    int body_len = esp_http_client_read_response(http_client, resp_buf, sizeof(resp_buf) - 1);
    esp_http_client_cleanup(http_client);
    mb_free(&mb);

    if (body_len < 0) {
        ESP_LOGE(TAG, "HTTP read failed");
        return false;
    }
    resp_buf[body_len] = '\0';

    if (status_code < 200 || status_code >= 300) {
        ESP_LOGE(TAG, "Enrollment failed: HTTP %d", status_code);
        return false;
    }

    /* Parse JSON response */
    prov_credentials_t new_creds;
    memset(&new_creds, 0, sizeof(new_creds));

    cJSON* root = cJSON_Parse(resp_buf);
    if (!root) {
        ESP_LOGE(TAG, "Enrollment response is not valid JSON");
        return false;
    }

    cJSON* policy = cJSON_GetObjectItemCaseSensitive(root, "policy");
    if (!cJSON_IsObject(policy)) {
        policy = root;
    }

    cJSON* field = cJSON_GetObjectItemCaseSensitive(root, "device_id");
    if (cJSON_IsString(field) && field->valuestring) {
        snprintf(new_creds.device_id, sizeof(new_creds.device_id), "%s", field->valuestring);
    }
    field = cJSON_GetObjectItemCaseSensitive(root, "client_certificate_pem");
    if (!cJSON_IsString(field) || !field->valuestring) {
        field = cJSON_GetObjectItemCaseSensitive(root, "certificate_pem");
    }
    if (cJSON_IsString(field) && field->valuestring) {
        snprintf(new_creds.client_certificate_pem,
                 sizeof(new_creds.client_certificate_pem), "%s", field->valuestring);
    }
    snprintf(new_creds.client_private_key_pem,
             sizeof(new_creds.client_private_key_pem), "%s", private_key_pem);

    field = cJSON_GetObjectItemCaseSensitive(root, "command_public_key_pem");
    if (!cJSON_IsString(field) || !field->valuestring) {
        field = cJSON_GetObjectItemCaseSensitive(root, "oms_command_public_key_pem");
    }
    if (cJSON_IsString(field) && field->valuestring) {
        snprintf(new_creds.command_public_key_pem,
                 sizeof(new_creds.command_public_key_pem), "%s", field->valuestring);
    } else {
        snprintf(new_creds.command_public_key_pem,
                 sizeof(new_creds.command_public_key_pem), "%s", kOmsCommandPubKeyPem);
    }

    field = cJSON_GetObjectItemCaseSensitive(policy, "mqtt_topic_for_firmware");
    if (cJSON_IsString(field) && field->valuestring) {
        snprintf(new_creds.mqtt_firmware_topic,
                 sizeof(new_creds.mqtt_firmware_topic), "%s", field->valuestring);
    }
    field = cJSON_GetObjectItemCaseSensitive(policy, "mqtt_topic_for_pings");
    if (cJSON_IsString(field) && field->valuestring) {
        snprintf(new_creds.mqtt_pings_topic,
                 sizeof(new_creds.mqtt_pings_topic), "%s", field->valuestring);
    }
    field = cJSON_GetObjectItemCaseSensitive(policy, "mqtt_broker_host");
    if (cJSON_IsString(field) && field->valuestring) {
        snprintf(new_creds.mqtt_broker_host,
                 sizeof(new_creds.mqtt_broker_host), "%s", field->valuestring);
    }
    field = cJSON_GetObjectItemCaseSensitive(policy, "mqtt_broker_port");
    if (cJSON_IsNumber(field)) {
        new_creds.mqtt_broker_port = (uint16_t)field->valuedouble;
    }
    field = cJSON_GetObjectItemCaseSensitive(policy, "mqtt_broker_use_tls");
    if (cJSON_IsBool(field)) {
        new_creds.mqtt_broker_use_tls = cJSON_IsTrue(field);
    }
    field = cJSON_GetObjectItemCaseSensitive(policy, "asset_id");
    if (!cJSON_IsString(field) || !field->valuestring) {
        field = cJSON_GetObjectItemCaseSensitive(root, "asset_id");
    }
    if (cJSON_IsString(field) && field->valuestring) {
        snprintf(new_creds.asset_id, sizeof(new_creds.asset_id), "%s", field->valuestring);
    }

    cJSON_Delete(root);

    if (new_creds.device_id[0] == '\0' ||
        new_creds.client_certificate_pem[0] == '\0' ||
        new_creds.client_private_key_pem[0] == '\0') {
        ESP_LOGE(TAG, "Enrollment response missing device_id or client certificate bundle");
        return false;
    }

    ESP_LOGI(TAG, "Enrolled as %s cert_len=%u key_len=%u cmd_pub_len=%u",
             new_creds.device_id,
             (unsigned)strlen(new_creds.client_certificate_pem),
             (unsigned)strlen(new_creds.client_private_key_pem),
             (unsigned)strlen(new_creds.command_public_key_pem));

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
