/*
 * OTA update for ESP32-C6 lock device.
 * Downloads firmware, verifies SHA-256, verifies ECDSA signature,
 * then performs OTA partition swap.
 */

#ifndef FORGEKEY_OTA_UPDATE_H
#define FORGEKEY_OTA_UPDATE_H

#include <stdbool.h>
#include <stdint.h>

/* OTA status callback */
typedef void (*ota_status_cb_t)(const char* state, const char* version,
                                 int progress, const char* error);

/* Parse an OTA dispatch JSON payload.
 * Returns true if all required fields are present and valid. */
bool ota_parse_dispatch(const uint8_t* payload, uint32_t length,
                        char* out_url, size_t url_len,
                        char* out_sha256, size_t sha256_len,
                        char* out_signature, size_t sig_len,
                        char* out_version, size_t ver_len,
                        bool* out_mandatory);

/* Apply an OTA update. Downloads, verifies, and reboots.
 * Does not return on success (device reboots).
 * Returns false on verification failure. */
bool ota_apply(const char* url, const char* sha256, const char* signature,
               const char* version, bool mandatory,
               ota_status_cb_t status_cb);

/* Mark the running partition as valid (call after first successful publish post-reboot). */
void ota_mark_stable(void);

/* Check if an OTA update is in progress. */
bool ota_in_progress(void);

#endif /* FORGEKEY_OTA_UPDATE_H */
