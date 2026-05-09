/*
 * Credential rotation for ESP32-C6 lock device.
 * Handles MQTT config topic: {"provisioning_token": "...", "valid_after": "..."}
 */

#ifndef FORGEKEY_CREDENTIAL_ROTATION_H
#define FORGEKEY_CREDENTIAL_ROTATION_H

#include <stdint.h>
#include <stdbool.h>

/* Handle a config message from MQTT. Returns true if the message was handled. */
bool credential_rotation_handle_config(const char* topic, const uint8_t* payload, uint32_t length);

/* Get the config topic for a given MAC. */
void credential_rotation_get_topic(char* topic_out, size_t topic_len, const char* mac);

#endif /* FORGEKEY_CREDENTIAL_ROTATION_H */
