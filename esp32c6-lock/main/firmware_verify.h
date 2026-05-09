/*
 * Firmware signature verification header for ESP32-C6 lock build.
 */

#ifndef FORGEKEY_FIRMWARE_VERIFY_H
#define FORGEKEY_FIRMWARE_VERIFY_H

#include <stdint.h>
#include <stdbool.h>

bool firmware_verify_signature(const uint8_t* sha256_digest, size_t digest_len,
                               const uint8_t* signature_der, size_t signature_len);

bool firmware_verify_decode_base64(const char* in, size_t in_len,
                                    uint8_t* out, size_t* out_len);

#endif /* FORGEKEY_FIRMWARE_VERIFY_H */
