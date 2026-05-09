/*
 * Provisioning for ESP32-C6 lock device.
 * NVS-backed credential storage + OMS registration via esp_http_client.
 */

#ifndef FORGEKEY_PROVISIONING_H
#define FORGEKEY_PROVISIONING_H

#include <stdbool.h>
#include <stdint.h>

#define FORGEKEY_PROV_MAX_DEVICES_ID 64
#define FORGEKEY_PROV_MAX_TOPIC      128
#define FORGEKEY_PROV_MAX_JWT        512
#define FORGEKEY_PROV_MAX_BROKER     64
#define FORGEKEY_PROV_MAX_ASSET_ID   64

typedef struct {
    char device_id[FORGEKEY_PROV_MAX_DEVICES_ID];
    char mqtt_firmware_topic[FORGEKEY_PROV_MAX_TOPIC];
    char mqtt_pings_topic[FORGEKEY_PROV_MAX_TOPIC];
    char jwt_token[FORGEKEY_PROV_MAX_JWT];
    char mqtt_broker_host[FORGEKEY_PROV_MAX_BROKER];
    uint16_t mqtt_broker_port;
    bool mqtt_broker_use_tls;
    char asset_id[FORGEKEY_PROV_MAX_ASSET_ID];
} prov_credentials_t;

/* Initialize NVS-backed storage and increment boot counter. */
void provisioning_begin(void);

/* True if device has been provisioned (dev_id + jwt present). */
bool provisioning_is_provisioned(void);

/* Load stored credentials from NVS. */
prov_credentials_t provisioning_credentials(void);

/* Persist credentials to NVS. */
bool provisioning_persist(const prov_credentials_t* creds);

/* Wipe stored credentials from NVS. */
void provisioning_clear(void);

/* Get active provisioning token (NVS override or compile-time default). */
const char* provisioning_active_token(void);

/* Set provisioning token (delivered via MQTT config). */
bool provisioning_set_token(const char* token);

/* POST registration to OMS. Returns true on success.
 * mac: lowercase hex MAC (12 chars).
 * ip_addr: device IP address string.
 * jpeg_buf/jpeg_len: camera photo data (NULL/0 for lock build). */
bool provisioning_register(const char* host, uint16_t port,
                           const char* mac, const char* ip_addr,
                           const uint8_t* jpeg_buf, size_t jpeg_len);

/* Get the boot count. */
uint32_t provisioning_boot_count(void);

/* Get the asset ID from provisioning (empty string if not available). */
const char* provisioning_get_asset_id(void);

#endif /* FORGEKEY_PROVISIONING_H */
