#ifndef OMS_COMMAND_PUBKEY_H
#define OMS_COMMAND_PUBKEY_H

// PEM-encoded ECDSA P-256 public key whose private counterpart lives on the
// OMS server and signs lock/operator command JWTs. Devices may also receive a
// replacement key during enrollment; this built-in copy is the fallback when
// NVS does not yet contain an override.

#ifndef FORGEKEY_OMS_COMMAND_PUBKEY_PEM
#define FORGEKEY_OMS_COMMAND_PUBKEY_PEM \
"-----BEGIN PUBLIC KEY-----\n" \
"MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEREPLACEMEREPLACEMEREPLACEMEREPLA\n" \
"CEMEREPLACEMEREPLACEMEREPLACEMEREPLACEMEREPLACEMEREPLACEMEREPLAC=\n" \
"-----END PUBLIC KEY-----\n"
#endif

extern const char* kOmsCommandPubKeyPem;

#endif
