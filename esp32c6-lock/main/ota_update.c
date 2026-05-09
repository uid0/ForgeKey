/*
 * OTA update implementation for ESP32-C6 lock device.
 * Downloads firmware, verifies SHA-256, verifies ECDSA signature,
 * then performs OTA partition swap.
 */

#include "ota_update.h"
#include "firmware_verify.h"
#include "oms_ca.h"
#include "device_config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_partition.h"
#include "mbedtls/sha256.h"

static const char* TAG = "OTA";

static bool s_in_progress = false;

/* Helper: hex string comparison (case-insensitive) */
static bool hex_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    size_t len_a = strlen(a);
    size_t len_b = strlen(b);
    if (len_a != len_b) return false;
    for (size_t i = 0; i < len_a; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

/* Helper: to-hex */
static void to_hex(const uint8_t* in, size_t n, char* out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hex[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[in[i] & 0xF];
    }
    out[n * 2] = '\0';
}

/* Helper: split URL into scheme, host, port, path */
static bool split_url(const char* url, bool* out_tls, char* host, size_t host_len,
                      uint16_t* out_port, char* path, size_t path_len) {
    const char* scheme_end = strstr(url, "://");
    if (!scheme_end) return false;

    if (strncmp(url, "https", 5) == 0) {
        *out_tls = true;
        *out_port = 443;
    } else if (strncmp(url, "http", 4) == 0) {
        *out_tls = false;
        *out_port = 80;
    } else {
        return false;
    }

    const char* host_start = scheme_end + 3;
    const char* path_start = strchr(host_start, '/');
    size_t host_port_len = path_start ? (path_start - host_start) : strlen(host_start);

    const char* colon = memchr(host_start, ':', host_port_len);
    if (colon) {
        strncpy(host, host_start, colon - host_start);
        host[colon - host_start] = '\0';
        *out_port = (uint16_t)atoi(colon + 1);
    } else {
        strncpy(host, host_start, host_len - 1);
        host[host_len - 1] = '\0';
    }

    if (path_start) {
        strncpy(path, path_start, path_len - 1);
        path[path_len - 1] = '\0';
    } else {
        strncpy(path, "/", path_len);
    }

    return strlen(host) > 0;
}

bool ota_parse_dispatch(const uint8_t* payload, uint32_t length,
                        char* out_url, size_t url_len,
                        char* out_sha256, size_t sha256_len,
                        char* out_signature, size_t sig_len,
                        char* out_version, size_t ver_len,
                        bool* out_mandatory) {
    /* Simple JSON field extraction */
    char payload_copy[length + 1];
    memcpy(payload_copy, payload, length);
    payload_copy[length] = '\0';

    /* Find "url" */
    char* url_key = strstr(payload_copy, "\"url\"");
    if (!url_key) {
        ESP_LOGE(TAG, "OTA parse: missing url");
        return false;
    }
    char* url_colon = strchr(url_key + 4, ':');
    if (!url_colon) return false;
    char* url_q1 = strchr(url_colon + 1, '"');
    if (!url_q1) return false;
    char* url_q2 = strchr(url_q1 + 1, '"');
    if (!url_q2) return false;
    size_t url_len_val = url_q2 - url_q1 - 1;
    if (url_len_val >= url_len) return false;
    strncpy(out_url, url_q1 + 1, url_len_val);
    out_url[url_len_val] = '\0';

    /* Find "sha256" */
    char* sha_key = strstr(payload_copy, "\"sha256\"");
    if (!sha_key) {
        ESP_LOGE(TAG, "OTA parse: missing sha256");
        return false;
    }
    char* sha_colon = strchr(sha_key + 7, ':');
    if (!sha_colon) return false;
    char* sha_q1 = strchr(sha_colon + 1, '"');
    if (!sha_q1) return false;
    char* sha_q2 = strchr(sha_q1 + 1, '"');
    if (!sha_q2) return false;
    size_t sha_len_val = sha_q2 - sha_q1 - 1;
    if (sha_len_val != 64) {
        ESP_LOGE(TAG, "OTA parse: sha256 length %zu != 64", sha_len_val);
        return false;
    }
    if (sha_len_val >= sha256_len) return false;
    strncpy(out_sha256, sha_q1 + 1, sha_len_val);
    out_sha256[sha_len_val] = '\0';

    /* Find "signature" */
    char* sig_key = strstr(payload_copy, "\"signature\"");
    if (!sig_key) {
        ESP_LOGE(TAG, "OTA parse: missing signature");
        return false;
    }
    char* sig_colon = strchr(sig_key + 10, ':');
    if (!sig_colon) return false;
    char* sig_q1 = strchr(sig_colon + 1, '"');
    if (!sig_q1) return false;
    char* sig_q2 = strchr(sig_q1 + 1, '"');
    if (!sig_q2) return false;
    size_t sig_len_val = sig_q2 - sig_q1 - 1;
    if (sig_len_val >= sig_len) return false;
    strncpy(out_signature, sig_q1 + 1, sig_len_val);
    out_signature[sig_len_val] = '\0';

    /* Find "version" */
    char* ver_key = strstr(payload_copy, "\"version\"");
    if (!ver_key) {
        ESP_LOGE(TAG, "OTA parse: missing version");
        return false;
    }
    char* ver_colon = strchr(ver_key + 9, ':');
    if (!ver_colon) return false;
    char* ver_q1 = strchr(ver_colon + 1, '"');
    if (!ver_q1) return false;
    char* ver_q2 = strchr(ver_q1 + 1, '"');
    if (!ver_q2) return false;
    size_t ver_len_val = ver_q2 - ver_q1 - 1;
    if (ver_len_val >= ver_len) return false;
    strncpy(out_version, ver_q1 + 1, ver_len_val);
    out_version[ver_len_val] = '\0';

    /* Find "mandatory" */
    char* man_key = strstr(payload_copy, "\"mandatory\"");
    if (man_key) {
        char* man_colon = strchr(man_key + 12, ':');
        if (man_colon) {
            char* val = man_colon + 1;
            while (*val == ' ' || *val == '"') val++;
            *out_mandatory = (*val == 't');
        }
    } else {
        *out_mandatory = false;
    }

    return true;
}

bool ota_apply(const char* url, const char* sha256, const char* signature,
               const char* version, bool mandatory,
               ota_status_cb_t status_cb) {
    s_in_progress = true;

    if (status_cb) status_cb("received", version, -1, NULL);

    bool tls = false;
    char host[64] = {0};
    uint16_t port = 0;
    char path[256] = {0};

    if (!split_url(url, &tls, host, sizeof(host), &port, path, sizeof(path))) {
        ESP_LOGE(TAG, "OTA: malformed URL");
        if (status_cb) status_cb("failed", version, -1, "malformed_url");
        s_in_progress = false;
        return false;
    }

    if (status_cb) status_cb("downloading", version, 0, NULL);

    /* Use esp_https_ota for the OTA download */
    esp_http_client_config_t http_cfg = {
        .url = url,
        .cert_pem = kOmsCaPem,
        .timeout_ms = 30000,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &https_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        if (status_cb) status_cb("failed", version, -1, "update_begin_failed");
        s_in_progress = false;
        return false;
    }

    ESP_LOGI(TAG, "OTA downloading version %s", version);

    /* Download and stream SHA-256 */
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);

    /* We need to read the HTTP body to compute SHA-256.
     * esp_https_ota_perform does the download internally but doesn't expose
     * the raw bytes. We'll use esp_http_client directly for streaming. */

    esp_http_client_handle_t http_client = esp_http_client_init(&http_cfg);
    esp_http_client_set_method(http_client, HTTP_METHOD_GET);
    esp_http_client_set_header(http_client, "User-Agent", "ForgeKey-OTA/1");
    esp_http_client_set_header(http_client, "Connection", "close");

    err = esp_http_client_perform(http_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA HTTP failed: %s", esp_err_to_name(err));
        mbedtls_sha256_free(&sha_ctx);
        esp_http_client_cleanup(http_client);
        esp_https_ota_abort(https_ota_handle);
        if (status_cb) status_cb("failed", version, -1, "http_error");
        s_in_progress = false;
        return false;
    }

    int status_code = esp_http_client_get_status_code(http_client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "OTA HTTP status %d", status_code);
        mbedtls_sha256_free(&sha_ctx);
        esp_http_client_cleanup(http_client);
        esp_https_ota_abort(https_ota_handle);
        char err_buf[32];
        snprintf(err_buf, sizeof(err_buf), "http_%d", status_code);
        if (status_cb) status_cb("failed", version, -1, err_buf);
        s_in_progress = false;
        return false;
    }

    int content_length = esp_http_client_get_content_length(http_client);
    if (content_length <= 0) {
        ESP_LOGE(TAG, "OTA: no Content-Length");
        mbedtls_sha256_free(&sha_ctx);
        esp_http_client_cleanup(http_client);
        esp_https_ota_abort(https_ota_handle);
        if (status_cb) status_cb("failed", version, -1, "no_content_length");
        s_in_progress = false;
        return false;
    }

    ESP_LOGI(TAG, "OTA: downloading %d bytes", content_length);

    /* Stream body to OTA partition and SHA-256 */
    int received = 0;
    int last_progress = 0;
    uint8_t buf[1024];

    while (received < content_length) {
        int n = esp_http_client_read(http_client, (char*)buf, sizeof(buf));
        if (n <= 0) {
            if (esp_http_client_is_complete_data_transfer(http_client)) {
                break;
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
            continue;
        }

        err = esp_https_ota_write_img(https_ota_handle, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
            mbedtls_sha256_free(&sha_ctx);
            esp_http_client_cleanup(http_client);
            esp_https_ota_abort(https_ota_handle);
            if (status_cb) status_cb("failed", version, -1, "flash_write_failed");
            s_in_progress = false;
            return false;
        }

        mbedtls_sha256_update(&sha_ctx, buf, n);
        received += n;

        int pct = (int)((received * 100) / content_length);
        if (pct >= last_progress + 10 && pct < 100) {
            last_progress = pct;
            if (status_cb) status_cb("downloading", version, pct, NULL);
        }
    }

    mbedtls_sha256_free(&sha_ctx);
    esp_http_client_cleanup(http_client);

    /* Get SHA-256 digest */
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha_ctx, digest);
    mbedtls_sha256_free(&sha_ctx);

    if (status_cb) status_cb("verifying", version, 100, NULL);

    char actual_hex[65];
    to_hex(digest, 32, actual_hex);

    if (!hex_eq(actual_hex, sha256)) {
        ESP_LOGE(TAG, "OTA: SHA-256 mismatch (got %s, expected %s)", actual_hex, sha256);
        esp_https_ota_abort(https_ota_handle);
        if (status_cb) status_cb("failed", version, -1, "sha256_mismatch");
        s_in_progress = false;
        return false;
    }

    /* Verify ECDSA signature */
    uint8_t sig_buf[128];
    size_t sig_len = sizeof(sig_buf);
    if (!firmware_verify_decode_base64(signature, strlen(signature), sig_buf, &sig_len)) {
        ESP_LOGE(TAG, "OTA: signature is not valid base64");
        esp_https_ota_abort(https_ota_handle);
        if (status_cb) status_cb("failed", version, -1, "bad_base64_signature");
        s_in_progress = false;
        return false;
    }

    if (!firmware_verify_signature(digest, 32, sig_buf, sig_len)) {
        ESP_LOGE(TAG, "OTA: signature verification failed");
        esp_https_ota_abort(https_ota_handle);
        if (status_cb) status_cb("failed", version, -1, "signature_invalid");
        s_in_progress = false;
        return false;
    }

    ESP_LOGI(TAG, "OTA: signature verified");

    /* Finalize OTA and reboot */
    esp_https_ota_finish(https_ota_handle);

    if (status_cb) status_cb("rebooting", version, 100, NULL);
    ESP_LOGI(TAG, "OTA: %s installed, rebooting", version);

    s_in_progress = false;
    esp_restart();
    /* unreachable */
    return false;
}

void ota_mark_stable(void) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) return;

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) return;

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA: marked running partition as valid");
    } else {
        ESP_LOGE(TAG, "OTA: mark valid failed: 0x%x", err);
    }
}

bool ota_in_progress(void) {
    return s_in_progress;
}
