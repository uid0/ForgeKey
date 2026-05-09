/*
 * Firmware signature verification for ESP32-C6 lock build.
 * Uses mbedtls (included via ESP-IDF esp-tls component).
 */

#include "firmware_verify.h"
#include "firmware_pubkey.h"

#include <mbedtls/pk.h>
#include <mbedtls/base64.h>
#include <string.h>
#include <stdio.h>

bool firmware_verify_signature(const uint8_t* sha256_digest, size_t digest_len,
                               const uint8_t* signature_der, size_t signature_len) {
    if (!sha256_digest || digest_len != 32) return false;
    if (!signature_der || signature_len == 0) return false;

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    size_t pem_len = strlen(kFirmwarePubKeyPem) + 1;
    int rc = mbedtls_pk_parse_public_key(&pk,
                                           (const unsigned char*)kFirmwarePubKeyPem,
                                           pem_len);
    if (rc != 0) {
        printf("fwverify: pk_parse_public_key failed: -0x%04x\n", -rc);
        mbedtls_pk_free(&pk);
        return false;
    }

    rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256,
                           sha256_digest, digest_len,
                           signature_der, signature_len);
    mbedtls_pk_free(&pk);

    if (rc != 0) {
        printf("fwverify: signature verify failed: -0x%04x\n", -rc);
        return false;
    }
    return true;
}

bool firmware_verify_decode_base64(const char* in, size_t in_len,
                                    uint8_t* out, size_t* out_len) {
    if (!out || !out_len) return false;
    size_t produced = 0;
    int rc = mbedtls_base64_decode(out, *out_len, &produced,
                                    (const unsigned char*)in, in_len);
    if (rc != 0) {
        printf("fwverify: base64 decode failed: -0x%04x\n", -rc);
        return false;
    }
    *out_len = produced;
    return true;
}
