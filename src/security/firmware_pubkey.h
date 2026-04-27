#ifndef FIRMWARE_PUBKEY_H
#define FIRMWARE_PUBKEY_H

// PEM-encoded ECDSA P-256 public key whose private counterpart lives on the
// OMS server (env var FORGEKEY_FIRMWARE_SIGNING_KEY). The OTA flow refuses
// to flash any image whose accompanying signature does not verify against
// this key. Replace this default with the real public key before flashing
// production devices — the embedded placeholder will reject ALL signatures.

#ifndef FORGEKEY_FW_PUBKEY_PEM
#define FORGEKEY_FW_PUBKEY_PEM \
"-----BEGIN PUBLIC KEY-----\n" \
"MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEREPLACEMEREPLACEMEREPLACEMEREPLA\n" \
"CEMEREPLACEMEREPLACEMEREPLACEMEREPLACEMEREPLACEMEREPLACEMEREPLAC=\n" \
"-----END PUBLIC KEY-----\n"
#endif

extern const char* kFirmwarePubKeyPem;

#endif
