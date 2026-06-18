#ifndef FORGEKEY_CA_H
#define FORGEKEY_CA_H

// PEM-encoded OMS-internal root CA. Distinct from oms_ca.h's ISRG Root X1
// (which verifies the OMS *server* certificate when establishing HTTPS):
// this is the operator-generated root used to sign:
//
//   * device mTLS client certs handed out by /enroll/, and
//   * firmware-signing leaf certs shipped in the MQTT dispatch payload's
//     `signing_cert` field.
//
// firmware_verify::verifySignatureChained() validates an incoming leaf
// against this root before trusting its embedded public key to verify
// the firmware binary signature.
//
// To regenerate against the live OMS host run:
//
//     scripts/build/fetch-forgekey-ca.sh   # exports current OMS CA cert
//
// The default below is a placeholder that will REJECT every chain. Replace
// with the real CA PEM before flashing production devices, OR override via
// -DFORGEKEY_INTERNAL_CA_PEM at build time. Multiple roots can be
// concatenated for a planned CA-rotation window.
//
// A device that has the legacy embedded firmware pubkey (firmware_pubkey.h)
// but a placeholder CA here continues to verify firmware using the embedded
// pubkey path — chain verification only kicks in when the dispatch payload
// carries `signing_cert` AND the chain validates here. So a stale CA does
// NOT brick existing devices; it just keeps them on the embedded-pubkey path.

#ifndef FORGEKEY_INTERNAL_CA_PEM
#define FORGEKEY_INTERNAL_CA_PEM \
"-----BEGIN CERTIFICATE-----\n" \
"REPLACE_WITH_REAL_OMS_INTERNAL_CA_PEM_OR_DEFINE_AT_BUILD_TIME\n" \
"-----END CERTIFICATE-----\n"
#endif

extern const char* kForgekeyInternalCaPem;

#endif
