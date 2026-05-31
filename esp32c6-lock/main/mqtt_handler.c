/*
 * MQTT handler implementation for ESP32-C6 lock device.
 * ESP-IDF MQTT client with TLS, mutual TLS auth, topic management, and auto-reconnect.
 */

#include "mqtt_handler.h"
#include "oms_ca.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_idf_version.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"

#include "mqtt_client.h"

static const char* TAG = "MQTT";

/* MQTT client handle */
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_mqtt_connected = false;

/* Topics */
static char s_topic_prefix[FORGEKEY_MQTT_MAX_TOPIC] = {0};
static char s_command_topic[FORGEKEY_MQTT_MAX_TOPIC] = {0};
static char s_config_topic[FORGEKEY_MQTT_MAX_TOPIC] = {0};
static char s_firmware_topic[FORGEKEY_MQTT_MAX_TOPIC] = {0};
static char s_firmware_status_topic[FORGEKEY_MQTT_MAX_TOPIC] = {0};
static char s_status_topic[FORGEKEY_MQTT_MAX_TOPIC] = {0};
static char s_capabilities_topic[FORGEKEY_MQTT_MAX_TOPIC] = {0};
static char s_state_topic[FORGEKEY_MQTT_MAX_TOPIC] = {0};

/* Handlers */
static mqtt_message_handler_t s_command_handler = NULL;
static mqtt_message_handler_t s_config_handler = NULL;
static mqtt_message_handler_t s_firmware_handler = NULL;

/* Broker info */
static char s_broker_host[64] = {0};
static int s_broker_port = 1883;
static bool s_use_tls = false;
static char s_client_cert_pem[2048] = {0};
static char s_client_key_pem[2048] = {0};

/* Last reconnect attempt time (seconds since epoch) */
static time_t s_last_reconnect = 0;
static const char kUnexpectedDisconnectState[] =
    "{\"online\":false,\"reason\":\"unexpected_disconnect\"}";

#define MQTT_OUTBOUND_QUEUE_SIZE 8
#define MQTT_OUTBOUND_TOPIC_MAX FORGEKEY_MQTT_MAX_TOPIC
#define MQTT_OUTBOUND_PAYLOAD_MAX 768
#define MQTT_OUTBOUND_BASE_BACKOFF_MS 1000
#define MQTT_OUTBOUND_MAX_BACKOFF_MS 30000
static const char* kQueueNamespace = "mqtt_outq";

typedef struct {
    char topic[MQTT_OUTBOUND_TOPIC_MAX];
    char payload[MQTT_OUTBOUND_PAYLOAD_MAX];
    int qos;
    bool retain;
    bool critical;
    int64_t next_attempt_ms;
    uint8_t attempts;
} queued_publish_t;

static queued_publish_t s_outbound_queue[MQTT_OUTBOUND_QUEUE_SIZE];
static size_t s_outbound_count = 0;

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

static int64_t retry_backoff_ms(uint8_t attempts) {
    int64_t backoff = MQTT_OUTBOUND_BASE_BACKOFF_MS;
    for (uint8_t i = 0; i < attempts && backoff < MQTT_OUTBOUND_MAX_BACKOFF_MS; ++i) {
        backoff *= 2;
    }
    return backoff > MQTT_OUTBOUND_MAX_BACKOFF_MS ? MQTT_OUTBOUND_MAX_BACKOFF_MS : backoff;
}

static void persist_critical_queue(void) {
    nvs_handle_t nvs;
    if (nvs_open(kQueueNamespace, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_erase_all(nvs);
    uint8_t count = 0;
    for (size_t i = 0; i < s_outbound_count && count < MQTT_OUTBOUND_QUEUE_SIZE; ++i) {
        if (!s_outbound_queue[i].critical) continue;
        char key[8];
        snprintf(key, sizeof(key), "t%u", count);
        nvs_set_str(nvs, key, s_outbound_queue[i].topic);
        snprintf(key, sizeof(key), "p%u", count);
        nvs_set_str(nvs, key, s_outbound_queue[i].payload);
        snprintf(key, sizeof(key), "q%u", count);
        nvs_set_i32(nvs, key, s_outbound_queue[i].qos);
        snprintf(key, sizeof(key), "r%u", count);
        nvs_set_u8(nvs, key, s_outbound_queue[i].retain ? 1 : 0);
        count++;
    }
    nvs_set_u8(nvs, "count", count);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void load_critical_queue(void) {
    nvs_handle_t nvs;
    if (nvs_open(kQueueNamespace, NVS_READONLY, &nvs) != ESP_OK) return;
    uint8_t count = 0;
    if (nvs_get_u8(nvs, "count", &count) != ESP_OK) {
        nvs_close(nvs);
        return;
    }
    if (count > MQTT_OUTBOUND_QUEUE_SIZE) count = MQTT_OUTBOUND_QUEUE_SIZE;
    for (uint8_t i = 0; i < count && s_outbound_count < MQTT_OUTBOUND_QUEUE_SIZE; ++i) {
        queued_publish_t* item = &s_outbound_queue[s_outbound_count];
        char key[8];
        size_t topic_len = sizeof(item->topic);
        size_t payload_len = sizeof(item->payload);
        snprintf(key, sizeof(key), "t%u", i);
        if (nvs_get_str(nvs, key, item->topic, &topic_len) != ESP_OK) continue;
        snprintf(key, sizeof(key), "p%u", i);
        if (nvs_get_str(nvs, key, item->payload, &payload_len) != ESP_OK) continue;
        int32_t qos = 0;
        uint8_t retain = 0;
        snprintf(key, sizeof(key), "q%u", i);
        nvs_get_i32(nvs, key, &qos);
        snprintf(key, sizeof(key), "r%u", i);
        nvs_get_u8(nvs, key, &retain);
        item->qos = qos;
        item->retain = retain != 0;
        item->critical = true;
        item->attempts = 0;
        item->next_attempt_ms = 0;
        s_outbound_count++;
    }
    nvs_close(nvs);
    if (s_outbound_count) ESP_LOGI(TAG, "Loaded %u queued critical publishes", (unsigned)s_outbound_count);
}

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
        int msg_id = mqtt_handler_subscribe(topic, 1);
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

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");
            s_mqtt_connected = true;

            /* Resubscribe to all topics */
            subscribe_if_set(s_command_topic);
            subscribe_if_set(s_config_topic);
            subscribe_if_set(s_firmware_topic);
            {
                char ip_str[16];
                current_ip_string(ip_str, sizeof(ip_str));
                publish_state_json(true, ip_str, NULL);
            }
            mqtt_handler_tick();
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
                        const char* client_certificate_pem,
                        const char* client_private_key_pem,
                        bool use_tls) {
    strncpy(s_broker_host, broker_host, sizeof(s_broker_host) - 1);
    s_broker_port = port;
    s_use_tls = use_tls;
    s_client_cert_pem[0] = '\0';
    s_client_key_pem[0] = '\0';
    if (client_certificate_pem) {
        strncpy(s_client_cert_pem, client_certificate_pem, sizeof(s_client_cert_pem) - 1);
    }
    if (client_private_key_pem) {
        strncpy(s_client_key_pem, client_private_key_pem, sizeof(s_client_key_pem) - 1);
    }

    ESP_LOGI(TAG, "MQTT begin: broker=%s:%d tls=%d auth=mtls cert_len=%u key_len=%u",
             broker_host, port, (int)use_tls,
             (unsigned)strlen(s_client_cert_pem),
             (unsigned)strlen(s_client_key_pem));

    if (s_mqtt_client) {
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
    }

    esp_mqtt_client_config_t mqtt_cfg = {0};

#if ESP_IDF_VERSION_MAJOR >= 5
    mqtt_cfg.broker.address.hostname = broker_host;
    mqtt_cfg.broker.address.port = port;
    mqtt_cfg.credentials.authentication.certificate = client_certificate_pem;
    mqtt_cfg.credentials.authentication.key = client_private_key_pem;
    mqtt_cfg.session.keepalive = 60;
    mqtt_cfg.session.last_will.topic = s_state_topic[0] ? s_state_topic : NULL;
    mqtt_cfg.session.last_will.msg = kUnexpectedDisconnectState;
    mqtt_cfg.session.last_will.msg_len = sizeof(kUnexpectedDisconnectState) - 1;
    mqtt_cfg.session.last_will.qos = 0;
    mqtt_cfg.session.last_will.retain = true;
    mqtt_cfg.session.disable_clean_session = false;
    mqtt_cfg.session.reconnect_timeout_ms = 10000;
    mqtt_cfg.network.disable_auto_reconnect = false;
#else
    mqtt_cfg.host = broker_host;
    mqtt_cfg.port = port;
    mqtt_cfg.client_cert_pem = client_certificate_pem;
    mqtt_cfg.client_key_pem = client_private_key_pem;
    mqtt_cfg.keepalive = 60;
    mqtt_cfg.lwt_topic = s_state_topic[0] ? s_state_topic : NULL;
    mqtt_cfg.lwt_msg = kUnexpectedDisconnectState;
    mqtt_cfg.lwt_msg_len = sizeof(kUnexpectedDisconnectState) - 1;
    mqtt_cfg.lwt_qos = 0;
    mqtt_cfg.lwt_retain = true;
    mqtt_cfg.disable_clean_session = false;
    mqtt_cfg.disable_auto_reconnect = false;
#endif

    if (use_tls) {
#if ESP_IDF_VERSION_MAJOR >= 5
        mqtt_cfg.broker.verification.certificate = kOmsCaPem;
#else
        mqtt_cfg.cert_pem = kOmsCaPem;
#endif
        ESP_LOGI(TAG, "MQTT TLS enabled with CA pinning and client certificates");
    } else if (client_certificate_pem && client_certificate_pem[0]) {
        ESP_LOGW(TAG, "MQTT client certificate configured without TLS; broker will ignore it");
    }

    if (use_tls && (!client_certificate_pem || !client_certificate_pem[0] ||
                    !client_private_key_pem || !client_private_key_pem[0])) {
        ESP_LOGW(TAG, "MQTT mutual TLS requested but client cert/key missing");
    }

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt_client) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return false;
    }

    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);

    load_critical_queue();

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
    if (!s_mqtt_client || !s_mqtt_connected) return ESP_FAIL;
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, data, data_len, qos, retain);
    return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t mqtt_handler_publish_queued(const char* topic, const char* data,
                                       int data_len, int qos, bool retain,
                                       bool critical) {
    if (!topic || !topic[0] || !data) return ESP_FAIL;
    size_t payload_len = data_len >= 0 ? (size_t)data_len : strlen(data);
    if (payload_len >= MQTT_OUTBOUND_PAYLOAD_MAX) {
        ESP_LOGW(TAG, "Queued publish too large topic=%s len=%u", topic, (unsigned)payload_len);
        return ESP_FAIL;
    }
    if (mqtt_handler_publish(topic, data, (int)payload_len, qos, retain) == ESP_OK) return ESP_OK;

    if (s_outbound_count >= MQTT_OUTBOUND_QUEUE_SIZE) {
        size_t drop = 0;
        bool found_noncritical = false;
        for (size_t i = 0; i < s_outbound_count; ++i) {
            if (!s_outbound_queue[i].critical) { drop = i; found_noncritical = true; break; }
        }
        if (!found_noncritical && !critical) {
            ESP_LOGW(TAG, "Queue full of critical entries; dropping noncritical publish");
            return ESP_FAIL;
        }
        bool dropped_critical = s_outbound_queue[drop].critical;
        for (size_t i = drop + 1; i < s_outbound_count; ++i) s_outbound_queue[i - 1] = s_outbound_queue[i];
        s_outbound_count--;
        if (dropped_critical) persist_critical_queue();
    }

    queued_publish_t* item = &s_outbound_queue[s_outbound_count++];
    snprintf(item->topic, sizeof(item->topic), "%s", topic);
    memcpy(item->payload, data, payload_len);
    item->payload[payload_len] = '\0';
    item->qos = qos;
    item->retain = retain;
    item->critical = critical;
    item->attempts = 0;
    item->next_attempt_ms = now_ms();
    if (critical) persist_critical_queue();
    return ESP_OK;
}

void mqtt_handler_tick(void) {
    if (!s_mqtt_client || !s_mqtt_connected || s_outbound_count == 0) return;
    int64_t now = now_ms();
    bool critical_changed = false;
    for (size_t i = 0; i < s_outbound_count;) {
        queued_publish_t* item = &s_outbound_queue[i];
        if (now < item->next_attempt_ms) {
            i++;
            continue;
        }
        int msg_id = esp_mqtt_client_publish(s_mqtt_client, item->topic, item->payload, 0,
                                             item->qos, item->retain);
        if (msg_id >= 0) {
            ESP_LOGI(TAG, "Queued publish accepted topic=%s attempts=%u",
                     item->topic, (unsigned)item->attempts + 1);
            if (item->critical) critical_changed = true;
            for (size_t j = i + 1; j < s_outbound_count; ++j) s_outbound_queue[j - 1] = s_outbound_queue[j];
            s_outbound_count--;
            continue;
        }
        item->attempts++;
        item->next_attempt_ms = now + retry_backoff_ms(item->attempts);
        i++;
    }
    if (critical_changed) persist_critical_queue();
}

void mqtt_handler_set_command_handler(mqtt_message_handler_t handler) {
    s_command_handler = handler;
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
