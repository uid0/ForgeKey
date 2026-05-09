#!/usr/bin/env python3
"""Sync security headers from Arduino project to ESP32-C6 lock build.

Copies the real OMS CA cert and firmware pubkey from the Arduino project's
src/security/ directory into the ESP32-C6 lock project's main/ directory.

Run from repo root:
    python3 scripts/esp32c6-sync-security.py
"""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SRC_SECURITY = REPO_ROOT / "src" / "security"
DEST = REPO_ROOT / "esp32c6-lock" / "main"


def extract_pem_macro(header_path: Path, macro_name: str) -> str:
    """Extract the PEM string from a header's #define macro."""
    text = header_path.read_text()
    # Match the multi-line #define with \n-escaped newlines
    pattern = rf'#ifndef\s+{macro_name}\s*\n#define\s+{macro_name}\s+\\(.*?\\n.*?)\s*\n#endif'
    match = re.search(pattern, text, re.DOTALL)
    if not match:
        print(f"ERROR: Could not find {macro_name} in {header_path}")
        sys.exit(1)
    raw = match.group(1)
    # Unescape \n sequences to real newlines
    return raw.replace("\\n", "\n")


def write_oms_ca(dest: Path, pem: str):
    """Write OMS CA cert as a const char[] array."""
    # Convert PEM to C string literal
    c_str = '"\n'
    for line in pem.strip().split('\n'):
        c_str += f'"{line}\n"\n'
    c_str += '"\n'

    content = f'''/*
 * OMS CA certificate for ESP32-C6 lock build.
 * Auto-generated from src/security/oms_ca.h — do not edit manually.
 * Run scripts/esp32c6-sync-security.py to update.
 */

#ifndef FORGEKEY_OMS_CA_H
#define FORGEKEY_OMS_CA_H

const char kOmsCaPem[] = {c_str};

#endif /* FORGEKEY_OMS_CA_H */
'''
    dest.mkdir(parents=True, exist_ok=True)
    (dest / "oms_ca.h").write_text(content)
    print(f"  Written: {dest / 'oms_ca.h'} ({len(pem)} bytes)")


def write_firmware_pubkey(dest: Path, pem: str):
    """Write firmware pubkey as a const char[] array."""
    c_str = '"\n'
    for line in pem.strip().split('\n'):
        c_str += f'"{line}\n"\n'
    c_str += '"\n'

    content = f'''/*
 * Firmware signing public key for ESP32-C6 lock build.
 * Auto-generated from src/security/firmware_pubkey.h — do not edit manually.
 * Run scripts/esp32c6-sync-security.py to update.
 *
 * NOTE: This is a PLACEHOLDER. Replace with the real key before production.
 */

#ifndef FORGEKEY_FIRMWARE_PUBKEY_H
#define FORGEKEY_FIRMWARE_PUBKEY_H

const char kFirmwarePubKeyPem[] = {c_str};

#endif /* FORGEKEY_FIRMWARE_PUBKEY_H */
'''
    dest.mkdir(parents=True, exist_ok=True)
    (dest / "firmware_pubkey.h").write_text(content)
    print(f"  Written: {dest / 'firmware_pubkey.h'} ({len(pem)} bytes)")


def write_firmware_verify(dest: Path):
    """Write firmware_verify.c for ESP-IDF (mbedtls-based)."""
    content = r'''/*
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
'''
    dest.mkdir(parents=True, exist_ok=True)
    (dest / "firmware_verify.c").write_text(content)
    print(f"  Written: {dest / 'firmware_verify.c'}")


def write_firmware_verify_header(dest: Path):
    """Write firmware_verify.h for ESP-IDF."""
    content = r'''/*
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
'''
    dest.mkdir(parents=True, exist_ok=True)
    (dest / "firmware_verify.h").write_text(content)
    print(f"  Written: {dest / 'firmware_verify.h'}")


def main():
    print("Syncing security files from Arduino project to ESP32-C6 lock...")

    # Extract PEM from Arduino headers
    oms_ca_pem = extract_pem_macro(SRC_SECURITY / "oms_ca.h", "OMS_CA_PEM")
    fw_pubkey_pem = extract_pem_macro(SRC_SECURITY / "firmware_pubkey.h", "FORGEKEY_FW_PUBKEY_PEM")

    # Write to ESP32-C6 project
    write_oms_ca(DEST, oms_ca_pem)
    write_firmware_pubkey(DEST, fw_pubkey_pem)
    write_firmware_verify(DEST)
    write_firmware_verify_header(DEST)

    print("Done.")


if __name__ == "__main__":
    main()
