/*
 * WiFi setup for ESP32-C6 lock device.
 * Handles STA/AP mode, captive portal, and NVS-stored WiFi credentials.
 */

#include "wifi_setup.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "esp_http_server.h"
#include "esp_tls.h"

#include "device_config.h"

#ifdef FORGEKEY_WIFI_SECRETS_H
#include "wifi_secrets.h"
#endif

static const char* TAG = "WIFI";

/* WiFi event group */
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

/* Captive portal server */
static httpd_handle_t s_captive_server = NULL;

/* NVS key for stored WiFi credentials */
#define NVS_WIFI_NAMESPACE "wifi_creds"
#define NVS_KEY_SSID       "ssid"
#define NVS_KEY_PASSWORD   "password"

/* ===== WiFi event handler ===== */

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (xEventGroupGetBits(s_wifi_event_group) & WIFI_FAIL_BIT) {
            return;
        }
        esp_wifi_connect();
        ESP_LOGI(TAG, "Retrying to connect to AP...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ===== NVS WiFi credential helpers ===== */

static bool nvs_get_wifi_creds(char* ssid_out, size_t ssid_len,
                                char* password_out, size_t pass_len) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_WIFI_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "NVS wifi_creds namespace not found");
        return false;
    }

    size_t ssid_size = ssid_len;
    size_t pass_size = pass_len;
    esp_err_t err2 = nvs_get_str(nvs_handle, NVS_KEY_SSID, ssid_out, &ssid_size);
    esp_err_t err3 = nvs_get_str(nvs_handle, NVS_KEY_PASSWORD, password_out, &pass_size);

    nvs_close(nvs_handle);

    return (err2 == ESP_OK && err3 == ESP_OK);
}

static bool nvs_save_wifi_creds(const char* ssid, const char* password) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_WIFI_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS wifi_creds");
        return false;
    }

    nvs_set_str(nvs_handle, NVS_KEY_SSID, ssid);
    nvs_set_str(nvs_handle, NVS_KEY_PASSWORD, password);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "WiFi credentials saved to NVS");
    return true;
}

static void nvs_forget_wifi_creds(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_WIFI_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return;
    }
    nvs_erase_key(nvs_handle, NVS_KEY_SSID);
    nvs_erase_key(nvs_handle, NVS_KEY_PASSWORD);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "WiFi credentials erased from NVS");
}

/* ===== Predefined networks ===== */

#ifdef FORGEKEY_HAS_PREDEFINED_NETWORKS
static bool try_predefined_networks(void) {
    ESP_LOGI(TAG, "Trying predefined networks...");

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

#ifdef FORGEKEY_NETWORK_1_SSID
    strncpy((char*)wifi_config.sta.ssid, FORGEKEY_NETWORK_1_SSID, 32);
    strncpy((char*)wifi_config.sta.password, FORGEKEY_NETWORK_1_PASSWORD, 64);
    ESP_LOGI(TAG, "Trying network 1: %s", FORGEKEY_NETWORK_1_SSID);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, 15000 / portTICK_PERIOD_MS);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to predefined network: %s", FORGEKEY_NETWORK_1_SSID);
        return true;
    }
#endif

#ifdef FORGEKEY_NETWORK_2_SSID
    strncpy((char*)wifi_config.sta.ssid, FORGEKEY_NETWORK_2_SSID, 32);
    strncpy((char*)wifi_config.sta.password, FORGEKEY_NETWORK_2_PASSWORD, 64);
    ESP_LOGI(TAG, "Trying network 2: %s", FORGEKEY_NETWORK_2_SSID);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, 15000 / portTICK_PERIOD_MS);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to predefined network: %s", FORGEKEY_NETWORK_2_SSID);
        return true;
    }
#endif

    ESP_LOGI(TAG, "Predefined networks unavailable, falling back");
    return false;
}
#endif

/* ===== Captive portal ===== */

#define AP_SSID_PREFIX "ForgeKey-Setup-"
#define AP_MAX_SSID_LEN 32

static esp_err_t captive_portal_handler_get(esp_http_server_req_t req) {
    static const char* html =
        "<!DOCTYPE html>"
        "<html><head><title>ForgeKey Setup</title>"
        "<style>body{font-family:sans-serif;text-align:center;padding:50px}"
        "h1{color:#333}input{padding:10px;margin:5px;width:250px}"
        "button{padding:10px 20px;margin:5px;cursor:pointer}"
        ".error{color:red}</style></head><body>"
        "<h1>ForgeKey Setup</h1>"
        "<p>Enter your WiFi credentials:</p>"
        "<form method='POST'>"
        "SSID: <input type='text' name='ssid' required><br>"
        "Password: <input type='password' name='password' required><br>"
        "<button type='submit'>Save</button>"
        "</form>"
        "<p style='margin-top:30px;font-size:12px;color:#999'>"
        "Visit <a href='https://openmakersuite.com'>OpenMakerSuite</a> to generate unlock codes.</p>"
        "</body></html>";

    esp_http_server_respond(req, html, strlen(html));
    return ESP_OK;
}

static esp_err_t captive_portal_handler_post(esp_http_server_req_t req) {
    char ssid[64] = {0};
    char password[128] = {0};

    int content_len = esp_http_server_get_content_length(req);
    if (content_len <= 0 || content_len > 4096) {
        esp_http_server_set_status_code(req, HTTP_STATUS_BAD_REQUEST);
        esp_http_server_respond(req, "Bad request", 10);
        return ESP_OK;
    }

    char* body = malloc(content_len + 1);
    if (!body) {
        esp_http_server_set_status_code(req, HTTP_STATUS_INTERNAL_SERVER_ERROR);
        esp_http_server_respond(req, "Internal error", 14);
        return ESP_OK;
    }

    esp_http_server_read(req, body, content_len);
    body[content_len] = '\0';

    /* Parse URL-encoded form data */
    char* token = body;
    while (token && *token) {
        char* amp = strchr(token, '&');
        char* eq = strchr(token, '=');
        if (eq && (!amp || eq < amp)) {
            size_t key_len = eq - token;
            /* URL-decode key */
            for (size_t i = 0; i < key_len; i++) {
                if (token[i] == '+') token[i] = ' ';
            }
            if (key_len == 4 && strncmp(token, "ssid", 4) == 0) {
                char* val = eq + 1;
                char* val_amp = strchr(val, '&');
                size_t val_len = val_amp ? (val_amp - val) : strlen(val);
                /* URL-decode value */
                for (size_t i = 0; i < val_len; i++) {
                    if (val[i] == '+') val[i] = ' ';
                }
                strncpy(ssid, val, val_len);
                ssid[val_len] = '\0';
            } else if (key_len == 8 && strncmp(token, "password", 8) == 0) {
                char* val = eq + 1;
                char* val_amp = strchr(val, '&');
                size_t val_len = val_amp ? (val_amp - val) : strlen(val);
                for (size_t i = 0; i < val_len; i++) {
                    if (val[i] == '+') val[i] = ' ';
                }
                strncpy(password, val, val_len);
                password[val_len] = '\0';
            }
            token = amp ? amp + 1 : NULL;
        } else {
            break;
        }
    }
    free(body);

    if (strlen(ssid) > 0) {
        nvs_save_wifi_creds(ssid, password);
        esp_wifi_stop();
        esp_wifi_connect();
    }

    esp_http_server_set_status_code(req, HTTP_STATUS_FOUND);
    esp_http_server_set_header(req, "Location", "/");
    return ESP_OK;
}

static const httpd_uri_t captive_get = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = captive_portal_handler_get,
    .user_ctx = NULL
};

static const httpd_uri_t captive_post = {
    .uri = "/",
    .method = HTTP_POST,
    .handler = captive_portal_handler_post,
    .user_ctx = NULL
};

static esp_err_t captive_portal_start(const char* ap_ssid) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = HTTPD_CTRL_INVALID;
    config.max_uri_handlers = 4;

    wifi_config_t ap_config = {
        .ap = {
            .ssid_len = 0,
            .max_connection = 4,
            .password_min_len = 8,
        },
    };

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf((char*)ap_config.ap.ssid, AP_MAX_SSID_LEN,
             "%s%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);
    strncpy((char*)ap_config.ap.password, FORGEKEY_AP_PASSWORD, 63);
    ap_config.ap.password[63] = '\0';
    ap_config.ap.channel = 1;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_config.ap.ssid_hidden = 0;
    ap_config.ap.max_connection = 4;

    esp_netif_t* ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif) {
        ESP_LOGE(TAG, "Failed to create AP netif");
        return ESP_FAIL;
    }
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_start();

    if (httpd_start(&s_captive_server, &config) == ESP_OK) {
        httpd_register_uri_handler(s_captive_server, &captive_get);
        httpd_register_uri_handler(s_captive_server, &captive_post);
        ESP_LOGI(TAG, "Captive portal started on AP %s", ap_config.ap.ssid);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to start captive portal HTTP server");
    return ESP_FAIL;
}

static void captive_portal_stop(void) {
    if (s_captive_server) {
        httpd_stop(s_captive_server);
        s_captive_server = NULL;
    }
}

/* ===== Public API ===== */

bool wifi_setup_init(void) {
    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        ESP_LOGE(TAG, "Failed to create WiFi event group");
        return false;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t* sta_netif = esp_netif_create_default_wifi_sta();
    if (!sta_netif) {
        ESP_LOGE(TAG, "Failed to create STA netif");
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                    ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                    IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    ESP_LOGI(TAG, "WiFi initialized");
    return true;
}

bool wifi_connect_with_portal(unsigned long timeout_secs) {
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

#ifdef FORGEKEY_HAS_PREDEFINED_NETWORKS
    if (try_predefined_networks()) {
        return true;
    }
#endif

    /* Try NVS-stored credentials first */
    char ssid[64] = {0};
    char password[128] = {0};
    if (nvs_get_wifi_creds(ssid, sizeof(ssid), password, sizeof(password))) {
        wifi_config_t wifi_config = {
            .sta = {
                .ssid = {0},
                .password = {0},
                .threshold.authmode = WIFI_AUTH_WPA2_PSK,
                .pmf_cfg = {
                    .capable = true,
                    .required = false
                },
            },
        };
        strncpy((char*)wifi_config.sta.ssid, ssid, 32);
        strncpy((char*)wifi_config.sta.password, password, 64);
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        esp_wifi_connect();

        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE, pdFALSE, 30000 / portTICK_PERIOD_MS);

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Connected using NVS credentials");
            return true;
        }
        ESP_LOGI(TAG, "NVS credentials failed, starting captive portal");
    }

    /* Fall back to captive portal */
    char ap_ssid[AP_MAX_SSID_LEN];
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf(ap_ssid, sizeof(ap_ssid), "%s%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);

    captive_portal_start(ap_ssid);

    ESP_LOGI(TAG, "Waiting for WiFi credentials via captive portal (AP: %s)...", ap_ssid);
    vTaskDelay(timeout_secs * 1000 / portTICK_PERIOD_MS);

    captive_portal_stop();

    /* Try connecting again with potentially saved creds */
    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, timeout_secs * 1000 / portTICK_PERIOD_MS);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected after captive portal");
        return true;
    }

    ESP_LOGE(TAG, "WiFi connection failed");
    return false;
}

void wifi_forget_and_restart(void) {
    ESP_LOGI(TAG, "Forgetting WiFi credentials, restarting...");
    nvs_forget_wifi_creds();
    esp_wifi_stop();
    vTaskDelay(500 / portTICK_PERIOD_MS);
    esp_restart();
}
