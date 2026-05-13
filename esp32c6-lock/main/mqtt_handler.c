/*
 * MQTT handler implementation for ESP32-C6 lock device.
 * ESP-IDF MQTT client with TLS, JWT auth, topic management, and auto-reconnect.
 */

#include "mqtt_handler.h"
#include "oms_ca.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "nvs_flash.h"

#include "mqtt_client.h"

static const char* TAG = "MQTT";

/* MQTT event queue */
static QueueHandle_t s_mqtt_event_queue = NULL;

/* MQTT client handle */
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_mqtt_connected = false;

/* Topics */
static char s_topic_prefix[FORGEKEY_MQTT_MAX_TOPIC] = {0};
static char s_command_topic[FORGEKEY_MQTT_MAX_TOPIC] = {0};
static char s_lock_topic[FORGEKEY_MQTT_MAX_TOPIC] = {0};
static char s_config_topic[FORGEKEY_MQTT_MAX_TOPIC] = {0};
static char s_firmware_topic[FORGEKEY_MQTT_MAX_TOPIC] = {0};
static char s_firmware_status_topic[FORGEKEY_MQTT_MAX_TOPIC] = {0};
static char s_status_topic[FORGEKEY_MQTT_MAX_TOPIC] = {0};
static char s_capabilities_topic[FORGEKEY_MQTT_MAX_TOPIC] = {0};
static char s_state_topic[FORGEKEY_MQTT_MAX_TOPIC] = {0};

/* Handlers */
static mqtt_message_handler_t s_command_handler = NULL;
static mqtt_message_handler_t s_lock_handler = NULL;
static mqtt_message_handler_t s_config_handler = NULL;
static mqtt_message_handler_t s_firmware_handler = NULL;

/* Broker info */
static char s_broker_host[64] = {0};
static int s_broker_port = 1883;
static bool s_use_tls = false;
static char s_jwt_token[512] = {0};

/* Last reconnect attempt time (seconds since epoch) */
static time_t s_last_reconnect = 0;
static const char kUnexpectedDisconnectState[] =
    "{\"online\":false,\"reason\":\"unexpected_disconnect\"}";

static void build_state_payload(char* dest, size_t dest_size, bool online,
                                const char* ip, const char* reason) {
    if (!dest || dest_size == 0) {
        return;
    }

    if (ip && ip[0]) {
        snprintf(dest, dest_size, "{\"online\":%s,\"ip\":\"%s\"}",
                 online ? "true" : "false", ip);
        return;
    }
    if (reason && reason[0]) {
        snprintf(dest, dest_size, "{\"online\":%s,\"reason\":\"%s\"}",
                 online ? "true" : "false", reason);
        return;
    }
    snprintf(dest, dest_size, "{\"online\":%s}",
             online ? "true" : "false");
}

static void current_ip_string(char* dest, size_t dest_size) {
    if (!dest || dest_size == 0) {
        return;
    }
    dest[0] = '\0';

    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) {
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(sta, &ip_info) != ESP_OK) {
        return;
    }

    snprintf(dest, dest_size, IPSTR, IP2STR(&ip_info.ip));
}

static void publish_state_json(bool online, const char* ip, const char* reason) {
    if (!s_mqtt_client || !s_mqtt_connected) {
        return;
    }
    if (!s_state_topic[0]) {
        ESP_LOGW(TAG, "State topic unset; skipping online=%d", (int)online);
        return;
    }

    char payload[128];
    build_state_payload(payload, sizeof(payload), online, ip, reason);

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, s_state_topic,
                                         payload, 0, 0, true);
    ESP_LOGI(TAG, "State publish topic=%s payload=%s msg_id=%d",
             s_state_topic, payload, msg_id);
}

/* ===== MQTT event handler ===== */

static void subscribe_if_set(const char* topic) {
    if (topic && topic[0]) {
        int msg_id = mqtt_handler_subscribe(topic, 0);
        ESP_LOGI(TAG, "Subscribe topic=%s msg_id=%d", topic, msg_id);
    }
}

static void copy_topic(char* dest, size_t dest_size, const char* topic, int topic_len) {
    size_t copy_len = 0;
    if (dest_size == 0) return;
    if (topic && topic_len > 0) {
        copy_len = (size_t)topic_len;
        if (copy_len > dest_size - 1) copy_len = dest_size - 1;
        memcpy(dest, topic, copy_len);
    }
    dest[copy_len] = '\0';
}

static void mqtt_event_handler(void* handler_args, esp_event_base_t base,
                                int32_t event_id, void* event_data) {
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");
            s_mqtt_connected = true;

            /* Resubscribe to all topics */
            subscribe_if_set(s_command_topic);
            subscribe_if_set(s_lock_topic);
            subscribe_if_set(s_config_topic);
            subscribe_if_set(s_firmware_topic);
            {
                char ip_str[16];
                current_ip_string(ip_str, sizeof(ip_str));
                publish_state_json(true, ip_str, NULL);
            }
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            s_mqtt_connected = false;
            s_last_reconnect = time(NULL);
            break;

        case MQTT_EVENT_DATA:
            {
                char topic_buf[FORGEKEY_MQTT_MAX_TOPIC];
                copy_topic(topic_buf, sizeof(topic_buf), event->topic, event->topic_len);
                ESP_LOGI(TAG, "MQTT data on topic: %s", topic_buf);

                /* Dispatch to appropriate handler */
                if (s_command_handler && s_command_topic[0] &&
                    strcmp(topic_buf, s_command_topic) == 0) {
                    s_command_handler(topic_buf, (const uint8_t*)event->data, event->data_len);
                } else if (s_lock_handler && s_lock_topic[0] &&
                           strcmp(topic_buf, s_lock_topic) == 0) {
                    s_lock_handler(topic_buf, (const uint8_t*)event->data, event->data_len);
                } else if (s_config_handler && s_config_topic[0] &&
                           strcmp(topic_buf, s_config_topic) == 0) {
                    s_config_handler(topic_buf, (const uint8_t*)event->data, event->data_len);
                } else if (s_firmware_handler && s_firmware_topic[0] &&
                           strcmp(topic_buf, s_firmware_topic) == 0) {
                    s_firmware_handler(topic_buf, (const uint8_t*)event->data, event->data_len);
                } else {
                    ESP_LOGW(TAG, "Unhandled MQTT topic: %s", topic_buf);
                }
            }
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error");
            break;

        default:
            break;
    }
}

/* ===== Public API ===== */

bool mqtt_handler_begin(const char* broker_host, int port,
                        const char* jwt_token, bool use_tls) {
    strncpy(s_broker_host, broker_host, sizeof(s_broker_host) - 1);
    s_broker_port = port;
    s_use_tls = use_tls;
    if (jwt_token) {
        strncpy(s_jwt_token, jwt_token, sizeof(s_jwt_token) - 1);
    }

    ESP_LOGI(TAG, "MQTT begin: broker=%s:%d tls=%d jwt_len=%u",
             broker_host, port, (int)use_tls,
             jwt_token ? (unsigned)strlen(jwt_token) : 0);

    if (s_mqtt_client) {
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.hostname = broker_host,
        .broker.address.port = port,
        .credentials.username = "forgemqtt",
        .credentials.authentication.password = jwt_token,
        .session.keepalive = 60,
        .session.last_will.topic = s_state_topic[0] ? s_state_topic : NULL,
        .session.last_will.msg = kUnexpectedDisconnectState,
        .session.last_will.msg_len = sizeof(kUnexpectedDisconnectState) - 1,
        .session.last_will.qos = 0,
        .session.last_will.retain = true,
        .session.disable_clean_session = false,
        .session.reconnect_timeout_ms = 10000,
        .network.disable_auto_reconnect = false,
    };

    if (use_tls) {
        mqtt_cfg.broker.verification.certificate = kOmsCaPem;
        ESP_LOGI(TAG, "MQTT TLS enabled with CA pinning");
    }

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt_client) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return false;
    }

    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);

    esp_err_t err = esp_mqtt_client_start(s_mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

void mqtt_handler_set_topic_prefix(const char* mac) {
    snprintf(s_topic_prefix, sizeof(s_topic_prefix), "forgekey/%s/cabinet_lock", mac);
    snprintf(s_command_topic, sizeof(s_command_topic), "forgekey/%s/command", mac);
    snprintf(s_status_topic, sizeof(s_status_topic), "forgekey/%s/status", mac);
    snprintf(s_capabilities_topic, sizeof(s_capabilities_topic), "forgekey/%s/capabilities", mac);
    snprintf(s_state_topic, sizeof(s_state_topic), "forgekey/%s/state", mac);
    ESP_LOGI(TAG, "Topic prefix set: %s", s_topic_prefix);
}

int mqtt_handler_subscribe(const char* topic, int qos) {
    if (!s_mqtt_client) return -1;
    return esp_mqtt_client_subscribe(s_mqtt_client, topic, qos);
}

esp_err_t mqtt_handler_publish(const char* topic, const char* data,
                                int data_len, int qos, bool retain) {
    if (!s_mqtt_client) return ESP_FAIL;
    return esp_mqtt_client_publish(s_mqtt_client, topic, data, data_len, qos, retain);
}

void mqtt_handler_set_command_handler(mqtt_message_handler_t handler) {
    s_command_handler = handler;
}

void mqtt_handler_set_lock_handler(mqtt_message_handler_t handler) {
    s_lock_handler = handler;
}

void mqtt_handler_set_config_handler(mqtt_message_handler_t handler) {
    s_config_handler = handler;
}

void mqtt_handler_set_firmware_handler(mqtt_message_handler_t handler) {
    s_firmware_handler = handler;
}

void mqtt_handler_set_command_topic(const char* topic) {
    if (topic && topic[0]) {
        strncpy(s_command_topic, topic, sizeof(s_command_topic) - 1);
        s_command_topic[sizeof(s_command_topic) - 1] = '\0';
        ESP_LOGI(TAG, "Command topic set: %s", s_command_topic);
        if (s_mqtt_connected) {
            subscribe_if_set(s_command_topic);
        }
    }
}

void mqtt_handler_set_lock_topic(const char* topic) {
    if (topic && topic[0]) {
        strncpy(s_lock_topic, topic, sizeof(s_lock_topic) - 1);
        s_lock_topic[sizeof(s_lock_topic) - 1] = '\0';
        ESP_LOGI(TAG, "Lock topic set: %s", s_lock_topic);
        if (s_mqtt_connected) {
            subscribe_if_set(s_lock_topic);
        }
    }
}

void mqtt_handler_set_config_topic(const char* topic) {
    if (topic && topic[0]) {
        strncpy(s_config_topic, topic, sizeof(s_config_topic) - 1);
        s_config_topic[sizeof(s_config_topic) - 1] = '\0';
        ESP_LOGI(TAG, "Config topic set: %s", s_config_topic);
        if (s_mqtt_connected) {
            subscribe_if_set(s_config_topic);
        }
    }
}

void mqtt_handler_set_firmware_topic(const char* topic) {
    if (topic && topic[0]) {
        strncpy(s_firmware_topic, topic, sizeof(s_firmware_topic) - 1);
        s_firmware_topic[sizeof(s_firmware_topic) - 1] = '\0';
        ESP_LOGI(TAG, "Firmware topic set: %s", s_firmware_topic);
        if (!s_firmware_status_topic[0]) {
            snprintf(s_firmware_status_topic, sizeof(s_firmware_status_topic),
                     "%s/status", s_firmware_topic);
        }
        if (s_mqtt_connected) {
            subscribe_if_set(s_firmware_topic);
        }
    }
}

void mqtt_handler_set_status_topic(const char* topic) {
    if (topic && topic[0]) {
        strncpy(s_status_topic, topic, sizeof(s_status_topic) - 1);
        s_status_topic[sizeof(s_status_topic) - 1] = '\0';
        ESP_LOGI(TAG, "Status topic set: %s", s_status_topic);
    }
}

bool mqtt_handler_is_connected(void) {
    return s_mqtt_connected;
}

const char* mqtt_handler_get_status_topic(void) {
    return s_status_topic;
}

const char* mqtt_handler_get_lock_topic(void) {
    return s_lock_topic;
}

const char* mqtt_handler_get_command_topic(void) {
    return s_command_topic;
}

const char* mqtt_handler_get_firmware_status_topic(void) {
    return s_firmware_status_topic;
}

void mqtt_handler_set_firmware_status_topic(const char* topic) {
    if (topic && topic[0]) {
        strncpy(s_firmware_status_topic, topic, sizeof(s_firmware_status_topic) - 1);
        s_firmware_status_topic[sizeof(s_firmware_status_topic) - 1] = '\0';
    }
}

const char* mqtt_handler_get_capabilities_topic(void) {
    return s_capabilities_topic;
}

void mqtt_handler_end(void) {
    if (s_mqtt_client) {
        if (s_mqtt_connected) {
            publish_state_json(false, NULL, "graceful_disconnect");
            esp_mqtt_client_disconnect(s_mqtt_client);
        }
        esp_mqtt_client_stop(s_mqtt_client);
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        s_mqtt_connected = false;
    }
}
