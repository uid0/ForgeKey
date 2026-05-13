/*
 * MQTT handler for ESP32-C6 lock device.
 * ESP-IDF MQTT client with TLS, mutual TLS auth, topic management, and auto-reconnect.
 */

#ifndef FORGEKEY_MQTT_HANDLER_H
#define FORGEKEY_MQTT_HANDLER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define FORGEKEY_MQTT_MAX_TOPIC 128
#define FORGEKEY_MQTT_MAX_CLIENT_ID 64

typedef void (*mqtt_message_handler_t)(const char* topic, const uint8_t* payload, uint32_t length);

/* Initialize MQTT client with broker, port, client certificate/key, and TLS flag. */
bool mqtt_handler_begin(const char* broker_host, int port,
                        const char* client_certificate_pem,
                        const char* client_private_key_pem,
                        bool use_tls);

/* Set the MAC-derived topic prefix. */
void mqtt_handler_set_topic_prefix(const char* mac);

/* Subscribe to a topic. */
int mqtt_handler_subscribe(const char* topic, int qos);

/* Publish a message. Returns ESP_OK on success. */
esp_err_t mqtt_handler_publish(const char* topic, const char* data,
                                int data_len, int qos, bool retain);

/* Set handlers for specific topic types. */
void mqtt_handler_set_command_handler(mqtt_message_handler_t handler);
void mqtt_handler_set_lock_handler(mqtt_message_handler_t handler);
void mqtt_handler_set_config_handler(mqtt_message_handler_t handler);
void mqtt_handler_set_firmware_handler(mqtt_message_handler_t handler);

/* Set topic overrides. */
void mqtt_handler_set_command_topic(const char* topic);
void mqtt_handler_set_lock_topic(const char* topic);
void mqtt_handler_set_config_topic(const char* topic);
void mqtt_handler_set_firmware_topic(const char* topic);
void mqtt_handler_set_status_topic(const char* topic);

/* Check connection status. */
bool mqtt_handler_is_connected(void);

/* Get the status topic for publishing. */
const char* mqtt_handler_get_status_topic(void);

/* Get the lock topic for publishing. */
const char* mqtt_handler_get_lock_topic(void);

/* Get the standard command topic for subscriptions. */
const char* mqtt_handler_get_command_topic(void);

/* Get the firmware status topic. */
const char* mqtt_handler_get_firmware_status_topic(void);

/* Set firmware status topic. */
void mqtt_handler_set_firmware_status_topic(const char* topic);

/* Get the capabilities announcement topic. */
const char* mqtt_handler_get_capabilities_topic(void);

/* End MQTT client. */
void mqtt_handler_end(void);

#endif /* FORGEKEY_MQTT_HANDLER_H */
