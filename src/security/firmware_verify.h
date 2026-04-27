#ifndef FIRMWARE_VERIFY_H
#define FIRMWARE_VERIFY_H

#include <Arduino.h>
#include <stdint.h>

// ECDSA-over-SHA-256 signature verification for OTA images. The signature
// is the DER-encoded ECDSA(P-256) signature produced by OMS over the raw
// firmware bytes; the digest input is the same SHA-256 the OTA streamer
// computes for its existing integrity check.
//
// Returns true iff the signature is well-formed AND verifies against the
// baked-in public key (FORGEKEY_FW_PUBKEY_PEM). Any failure mode — parse
// error, key load error, signature mismatch — returns false; callers MUST
// treat false as "abort the OTA, do not flash".
namespace firmware_verify {

bool verifySignature(const uint8_t* sha256Digest, size_t digestLen,
                     const uint8_t* signatureDer, size_t signatureLen);

// Decode base64 (standard alphabet, optional padding). Writes up to *outLen
// bytes; sets *outLen to the number of bytes actually written. Returns false
// on malformed input.
bool decodeBase64(const String& in, uint8_t* out, size_t* outLen);

}  // namespace firmware_verify

#endif
