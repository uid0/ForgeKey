/*
 * ESP32-C6 Lock Device - Main application
 *
 * Cabinet-lock build for ForgeKey devices using ESP-IDF native APIs.
 * Implements the full device lifecycle:
 *   1. NVS init + GPIO init
 *   2. WiFi connect (with captive portal fallback)
 *   3. NTP sync
 *   4. OMS provisioning (first-boot registration)
 *   5. MQTT connect + topic subscription
 *   6. Web server + lock state machine + OTA
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_https_ota.h"
#include "esp_http_server.h"
#include "esp_tls.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "driver/gpio.h"

/* Lock configuration */
#include "lock_config.h"
#include "device_config.h"

/* Module headers */
#include "wifi_setup.h"
#include "provisioning.h"
#include "mqtt_handler.h"
#include "lock_state.h"
#include "web_server.h"
#include "ota_update.h"
#include "credential_rotation.h"

static const char* TAG = "LOCK";

#define LOCK_LOGI(fmt, ...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#define LOCK_LOGW(fmt, ...) ESP_LOGW(TAG, fmt, ##__VA_ARGS__)
#define LOCK_LOGE(fmt, ...) ESP_LOGE(TAG, fmt, ##__VA_ARGS__)

/* Forward declarations for MQTT handlers */
static void on_lock_command(const char* topic, const uint8_t* payload, uint32_t length);
static void on_config_message(const char* topic, const uint8_t* payload, uint32_t length);
static void on_firmware_dispatch(const char* topic, const uint8_t* payload, uint32_t length);

/* Application entry point */
void app_main(void) {
    LOCK_LOGI("ForgeKey Lock Starting...");
    LOCK_LOGI("Firmware version: %s", FORGEKEY_FIRMWARE_VERSION);
    LOCK_LOGI("ESP-IDF version: %s", esp_get_idf_version());

    /* ===== 1. NVS init ===== */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(ret);

    /* ===== 2. GPIO init (lock sensors) ===== */
    lock_state_gpio_init();
    lock_state_begin();

    /* ===== 3. WiFi init + connect ===== */
    LOCK_LOGI("Initializing WiFi...");
    ESP_ERROR_CHECK(wifi_setup_init());

    LOCK_LOGI("Connecting to WiFi (with captive portal fallback)...");
    if (!wifi_connect_with_portal(300)) {
        LOCK_LOGE("WiFi connection failed, restarting");
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        esp_restart();
    }
    LOCK_LOGI("WiFi connected");

    /* Get MAC address */
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char mac_str[13];
    snprintf(mac_str, sizeof(mac_str), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    lock_state_set_mac(mac_str);
    LOCK_LOGI("MAC Address: %s", mac_str);

    /* ===== 4. NTP sync ===== */
    LOCK_LOGI("Syncing NTP time...");
    configTzTime("UTC", "pool.ntp.org", "time.nist.gov");
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        LOCK_LOGW("NTP sync failed - TLS may fail");
    } else {
        LOCK_LOGI("NTP synced: %04d-%02d-%02d %02d:%02d:%02d",
                  timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    }

    /* ===== 5. Provisioning ===== */
    provisioning_begin();

    if (!provisioning_is_provisioned()) {
        LOCK_LOGI("First boot - registering with OMS");

        /* Get IP address */
        esp_netif_ip_info_t ip_info;
        esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info);
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));

        /* Register with OMS (no photo for lock build) */
        if (provisioning_register(OMS_HOST, OMS_PORT, mac_str, ip_str, NULL, 0)) {
            LOCK_LOGI("Registration successful");
        } else {
            LOCK_LOGW("Registration failed; will retry on next boot");
        }
    } else {
        LOCK_LOGI("Already provisioned as %s", provisioning_credentials().device_id);
    }

    /* Get credentials */
    prov_credentials_t creds = provisioning_credentials();
    const char* mqtt_jwt = creds.jwt_token[0] ? creds.jwt_token : "";

    /* ===== 6. MQTT connect ===== */
    /* Resolve broker info: NVS > fallback */
    const char* broker_host;
    int broker_port;
    bool broker_use_tls;

    if (creds.mqtt_broker_host[0] && creds.mqtt_broker_port != 0) {
        broker_host = creds.mqtt_broker_host;
        broker_port = creds.mqtt_broker_port;
        broker_use_tls = creds.mqtt_broker_use_tls;
        LOCK_LOGI("MQTT broker from NVS: %s:%d (tls=%d)",
                  broker_host, broker_port, (int)broker_use_tls);
    } else {
        broker_host = MQTT_BROKER_FALLBACK_HOST;
        broker_port = MQTT_BROKER_FALLBACK_PORT;
        broker_use_tls = MQTT_BROKER_FALLBACK_USE_TLS != 0;
        LOCK_LOGI("MQTT broker from fallback: %s:%d (tls=%d)",
                  broker_host, broker_port, (int)broker_use_tls);
    }

    if (!mqtt_handler_begin(broker_host, broker_port, mqtt_jwt, broker_use_tls)) {
        LOCK_LOGE("MQTT init failed");
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        esp_restart();
    }

    /* Set topic prefix */
    mqtt_handler_set_topic_prefix(mac_str);

    /* Set up topics */
    char lock_cmd_topic[128];
    char config_topic[128];
    snprintf(lock_cmd_topic, sizeof(lock_cmd_topic), "cabinets/%s/cmd", mac_str);
    credential_rotation_get_topic(config_topic, sizeof(config_topic), mac_str);

    mqtt_handler_set_lock_topic(lock_cmd_topic);
    mqtt_handler_set_config_topic(config_topic);
    mqtt_handler_set_lock_handler(on_lock_command);
    mqtt_handler_set_config_handler(on_config_message);

    /* Subscribe to firmware topic if available */
    if (creds.mqtt_firmware_topic[0]) {
        mqtt_handler_set_firmware_topic(creds.mqtt_firmware_topic);
        mqtt_handler_set_firmware_handler(on_firmware_dispatch);
        LOCK_LOGI("Firmware topic: %s", creds.mqtt_firmware_topic);
    }

    LOCK_LOGI("Setup complete. Starting main loop...");

    /* ===== 7. Web server init ===== */
    lock_web_server_init();

    /* ===== 8. Main loop ===== */
    uint32_t last_telemetry_ms = 0;
    bool mqtt_was_connected = false;
    bool capabilities_announced = false;

    while (1) {
        /* Tick lock state machine */
        lock_state_tick();

        /* Tick web server */
        lock_web_server_tick();

        /* MQTT connection monitoring */
        bool mqtt_connected = mqtt_handler_is_connected();
        if (mqtt_connected && !mqtt_was_connected) {
            LOCK_LOGI("MQTT connected");
            mqtt_was_connected = true;

            /* One-time capability announcement */
            if (!capabilities_announced) {
                cJSON* caps = cJSON_CreateArray();
                cJSON_AddItemToArray(caps, cJSON_CreateString("cabinet_lock"));
                cJSON* root = cJSON_CreateObject();
                cJSON_AddItemToObject(root, "capabilities", caps);
                cJSON_AddStringToObject(root, "firmware_version", FORGEKEY_FIRMWARE_VERSION);
                char* json_str = cJSON_PrintUnformatted(root);
                cJSON_Delete(root);

                if (json_str) {
                    const char* cap_topic = mqtt_handler_get_capabilities_topic();
                    if (cap_topic[0]) {
                        mqtt_handler_publish(cap_topic, json_str, strlen(json_str), 0, true);
                        LOCK_LOGI("Capabilities announced");
                    }
                    cJSON_free(json_str);
                }
                capabilities_announced = true;
            }
        } else if (!mqtt_connected && mqtt_was_connected) {
            LOCK_LOGW("MQTT disconnected");
            mqtt_was_connected = false;
        }

        /* Publish telemetry at interval */
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - last_telemetry_ms >= FORGEKEY_LOCK_TELEMETRY_INTERVAL_MS) {
            lock_telemetry_t tel = lock_state_get_telemetry();
            lock_trigger_t trigger = lock_state_get_last_trigger();

            const char* trigger_str = "unknown";
            switch (trigger) {
                case LOCK_TRIGGER_JWT: trigger_str = "jwt"; break;
                case LOCK_TRIGGER_MORTISE: trigger_str = "mortise"; break;
                case LOCK_TRIGGER_AUTO_UNLOCK: trigger_str = "auto_unlock"; break;
                case LOCK_TRIGGER_DOOR_CLOSE: trigger_str = "door_close"; break;
                case LOCK_TRIGGER_ALARM_TIMEOUT: trigger_str = "alarm_timeout"; break;
                default: trigger_str = "unknown"; break;
            }

            cJSON* root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "mac", mac_str);
            cJSON_AddBoolToObject(root, "secure", tel.secure);
            cJSON_AddBoolToObject(root, "item_present", !tel.ir_broken);
            cJSON_AddNumberToObject(root, "uptime", tel.uptime_ms);
            cJSON_AddStringToObject(root, "last_trigger", trigger_str);
            cJSON_AddStringToObject(root, "state", lock_state_state_name(lock_state_get_state()));
            cJSON_AddBoolToObject(root, "reed_closed", tel.reed_closed);
            cJSON_AddBoolToObject(root, "latch_locked", tel.latch_locked);
            cJSON_AddBoolToObject(root, "ir_broken", tel.ir_broken);
            cJSON_AddBoolToObject(root, "mortise_active", tel.mortise_active);

            char* json_str = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);

            if (json_str) {
                const char* status_topic = mqtt_handler_get_status_topic();
                if (status_topic[0]) {
                    mqtt_handler_publish(status_topic, json_str, strlen(json_str), 0, 0);
                    LOCK_LOGI("Telemetry published");
                }
                cJSON_free(json_str);
            }

            last_telemetry_ms = now_ms;
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

/* ===== MQTT message handlers ===== */

static void on_lock_command(const char* topic, const uint8_t* payload, uint32_t length) {
    LOCK_LOGI("Lock command on %.*s", (int)strlen(topic), topic);

    char payload_copy[length + 1];
    memcpy(payload_copy, payload, length);
    payload_copy[length] = '\0';

    cJSON* doc = cJSON_Parse(payload_copy);
    if (!doc) {
        LOCK_LOGW("Lock command: JSON parse error");
        return;
    }

    cJSON* token = cJSON_GetObjectItem(doc, "token");
    cJSON* timestamp = cJSON_GetObjectItem(doc, "timestamp");

    if (!token || !cJSON_IsString(token) || strlen(token->valuestring) == 0) {
        LOCK_LOGW("Lock command: missing token");
        cJSON_Delete(doc);
        return;
    }

    long ts = timestamp ? timestamp->valueint : 0;
    LOCK_LOGI("Lock command: token_len=%u ts=%ld",
              (unsigned)strlen(token->valuestring), ts);

    if (lock_state_handle_unlock(token->valuestring, ts)) {
        const char* status_topic = mqtt_handler_get_status_topic();
        if (status_topic[0]) {
            mqtt_handler_publish(status_topic,
                "{\"cmd_ack\":\"unlock\",\"state\":\"unlocked\"}", -1, 0, 0);
        }
        LOCK_LOGI("Lock command: unlock accepted");
    } else {
        const char* status_topic = mqtt_handler_get_status_topic();
        if (status_topic[0]) {
            mqtt_handler_publish(status_topic,
                "{\"cmd_ack\":\"unlock\",\"error\":\"invalid_token\"}", -1, 0, 0);
        }
        LOCK_LOGW("Lock command: unlock rejected");
    }

    cJSON_Delete(doc);
}

static void on_config_message(const char* topic, const uint8_t* payload, uint32_t length) {
    LOCK_LOGI("Config message on %.*s", (int)strlen(topic), topic);

    /* Try credential rotation first */
    if (credential_rotation_handle_config(topic, payload, length)) {
        return;
    }

    /* Check for "forget_wifi" command */
    char payload_copy[length + 1];
    memcpy(payload_copy, payload, length);
    payload_copy[length] = '\0';

    cJSON* doc = cJSON_Parse(payload_copy);
    if (!doc) {
        LOCK_LOGW("Config message: JSON parse error");
        return;
    }

    cJSON* cmd = cJSON_GetObjectItem(doc, "cmd");
    if (cmd && cJSON_IsString(cmd) && strcmp(cmd->valuestring, "forget_wifi") == 0) {
        LOCK_LOGI("Config: forget_wifi command");
        cJSON_Delete(doc);
        wifi_forget_and_restart();
        /* unreachable */
        return;
    }

    cJSON_Delete(doc);
    LOCK_LOGW("Config message: unhandled command");
}

static void on_firmware_dispatch(const char* topic, const uint8_t* payload, uint32_t length) {
    LOCK_LOGI("Firmware dispatch on %.*s (%u bytes)",
              (int)strlen(topic), topic, (unsigned)length);

    char url[256] = {0};
    char sha256[65] = {0};
    char signature[256] = {0};
    char version[32] = {0};
    bool mandatory = false;

    if (!ota_parse_dispatch(payload, length,
                            url, sizeof(url),
                            sha256, sizeof(sha256),
                            signature, sizeof(signature),
                            version, sizeof(version),
                            &mandatory)) {
        const char* status_topic = mqtt_handler_get_firmware_status_topic();
        if (status_topic[0]) {
            mqtt_handler_publish(status_topic,
                "{\"state\":\"failed\",\"error\":\"parse_error\"}", -1, 0, 0);
        }
        LOCK_LOGW("Firmware dispatch: parse error");
        return;
    }

    LOCK_LOGI("Firmware dispatch: version=%s mandatory=%d", version, (int)mandatory);

    const char* status_topic = mqtt_handler_get_firmware_status_topic();
    if (status_topic[0]) {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"state\":\"received\",\"version\":\"%s\"}", version);
        mqtt_handler_publish(status_topic, buf, -1, 0, 0);
    }

    LOCK_LOGI("Firmware dispatch: applying update");
    ota_apply(url, sha256, signature, version, mandatory, NULL);
    /* unreachable on success */
    LOCK_LOGW("Firmware dispatch: apply returned (failed)");
}
