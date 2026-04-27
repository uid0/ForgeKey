#include "firmware_verify.h"
#include "firmware_pubkey.h"

#include <mbedtls/pk.h>
#include <mbedtls/base64.h>
#include <string.h>

const char* kFirmwarePubKeyPem = FORGEKEY_FW_PUBKEY_PEM;

namespace firmware_verify {

bool verifySignature(const uint8_t* sha256Digest, size_t digestLen,
                     const uint8_t* signatureDer, size_t signatureLen) {
    if (!sha256Digest || digestLen != 32) return false;
    if (!signatureDer || signatureLen == 0) return false;

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    const size_t pemLen = strlen(kFirmwarePubKeyPem) + 1;  // +1 for NUL terminator (mbedtls requires it)
    int rc = mbedtls_pk_parse_public_key(&pk,
                                         (const unsigned char*)kFirmwarePubKeyPem,
                                         pemLen);
    if (rc != 0) {
        Serial.printf("fwverify: pk_parse_public_key failed: -0x%04x\n", -rc);
        mbedtls_pk_free(&pk);
        return false;
    }

    rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256,
                           sha256Digest, digestLen,
                           signatureDer, signatureLen);
    mbedtls_pk_free(&pk);

    if (rc != 0) {
        Serial.printf("fwverify: signature verify failed: -0x%04x\n", -rc);
        return false;
    }
    return true;
}

bool decodeBase64(const String& in, uint8_t* out, size_t* outLen) {
    if (!out || !outLen) return false;
    size_t produced = 0;
    int rc = mbedtls_base64_decode(out, *outLen, &produced,
                                   (const unsigned char*)in.c_str(),
                                   in.length());
    if (rc != 0) {
        Serial.printf("fwverify: base64 decode failed: -0x%04x\n", -rc);
        return false;
    }
    *outLen = produced;
    return true;
}

}  // namespace firmware_verify
