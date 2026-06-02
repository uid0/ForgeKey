#include "firmware_verify.h"
#include "firmware_pubkey.h"
#include "forgekey_ca.h"

#include <mbedtls/base64.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
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

bool verifySignatureChained(const uint8_t* sha256Digest, size_t digestLen,
                            const uint8_t* signatureDer, size_t signatureLen,
                            const char* leafCertPem) {
    if (!sha256Digest || digestLen != 32) return false;
    if (!signatureDer || signatureLen == 0) return false;
    if (!leafCertPem || leafCertPem[0] == '\0') {
        Serial.println("fwverify: chained verify called with empty leaf PEM");
        return false;
    }

    mbedtls_x509_crt root;
    mbedtls_x509_crt leaf;
    mbedtls_x509_crt_init(&root);
    mbedtls_x509_crt_init(&leaf);

    bool ok = false;
    do {
        const size_t rootLen = strlen(kForgekeyInternalCaPem) + 1;
        int rc = mbedtls_x509_crt_parse(&root,
                                        (const unsigned char*)kForgekeyInternalCaPem,
                                        rootLen);
        if (rc != 0) {
            // Negative rc on parse failure, positive rc = number of certs that
            // failed inside a concatenated PEM bundle. Either is fatal here:
            // if we can't trust the trust anchor, we can't trust anything.
            Serial.printf("fwverify: internal CA parse rc=%d (placeholder PEM?)\n", rc);
            break;
        }

        const size_t leafLen = strlen(leafCertPem) + 1;
        rc = mbedtls_x509_crt_parse(&leaf,
                                    (const unsigned char*)leafCertPem,
                                    leafLen);
        if (rc != 0) {
            Serial.printf("fwverify: leaf cert parse rc=%d\n", rc);
            break;
        }

        // mbedtls_x509_crt_verify does signature + validity-window + CA-chain
        // checks. The flags out-param tells us *which* checks failed; we don't
        // distinguish — any failure aborts the OTA.
        uint32_t flags = 0;
        rc = mbedtls_x509_crt_verify(&leaf, &root,
                                     /*ca_crl=*/nullptr,
                                     /*cn=*/nullptr,
                                     &flags,
                                     /*f_vrfy=*/nullptr,
                                     /*p_vrfy=*/nullptr);
        if (rc != 0) {
            char buf[256];
            mbedtls_x509_crt_verify_info(buf, sizeof(buf), "  ", flags);
            Serial.printf("fwverify: leaf chain verify failed flags=0x%08x rc=-0x%04x\n"
                          "%s", (unsigned)flags, -rc, buf);
            break;
        }

        // Pin the leaf to CODE_SIGNING — a CA-issued mTLS CLIENT_AUTH cert
        // must NOT be accepted as a firmware signer, even though it chains to
        // the same root. mbedtls returns 0 when the OID is present.
        rc = mbedtls_x509_crt_check_extended_key_usage(
            &leaf, MBEDTLS_OID_CODE_SIGNING, MBEDTLS_OID_SIZE(MBEDTLS_OID_CODE_SIGNING));
        if (rc != 0) {
            Serial.printf("fwverify: leaf cert missing CODE_SIGNING EKU rc=-0x%04x\n", -rc);
            break;
        }

        // Finally, verify the firmware-binary signature against the leaf's
        // public key. The leaf's mbedtls_pk_context is embedded; we use it
        // directly without copying.
        rc = mbedtls_pk_verify(&leaf.pk, MBEDTLS_MD_SHA256,
                               sha256Digest, digestLen,
                               signatureDer, signatureLen);
        if (rc != 0) {
            Serial.printf("fwverify: binary signature does not verify under "
                          "leaf pubkey rc=-0x%04x\n", -rc);
            break;
        }

        ok = true;
    } while (false);

    mbedtls_x509_crt_free(&leaf);
    mbedtls_x509_crt_free(&root);
    return ok;
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
