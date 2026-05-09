#!/usr/bin/env python3
"""Extract secrets from Arduino project and populate ESP32-C6 lock headers.

Reads provisioning tokens, CA certificates, firmware public keys, JWT secrets,
and predefined WiFi networks from the Arduino codebase, then writes populated
C headers into esp32c6-lock/main/.

Run from repo root:
    python3 scripts/esp32c6-extract-secrets.py
"""

import re
import sys
from pathlib import Path

# Navigate up two levels from this script to get repo root:
#   esp32c6-lock/scripts/extract-secrets.py -> esp32c6-lock/ -> repo root
_SCRIPT_DIR = Path(__file__).resolve().parent.parent
_REPO_ROOT = _SCRIPT_DIR.parent  # repo root = /Users/ian/Work/ForgeKey

SRC_SECURITY = _REPO_ROOT / "src" / "security"
SRC_PROVISIONING = _REPO_ROOT / "src" / "provisioning"
SRC_LOCK = _REPO_ROOT / "src" / "lock"
SRC_WIFI = _REPO_ROOT / "src" / "wifi_setup"
DEST = _REPO_ROOT / "esp32c6-lock" / "main"


def extract_pem_macro(header_path: Path, macro_name: str) -> str:
    """Extract the PEM string from a header's #define macro."""
    text = header_path.read_text()
    # Match the multi-line #define with \n-escaped newlines and line continuations
    pattern = rf'#ifndef\s+{macro_name}\s*\n#define\s+{macro_name}\s+\\(.*?\\n.*?)\s*\n#endif'
    match = re.search(pattern, text, re.DOTALL)
    if not match:
        print(f"ERROR: Could not find {macro_name} in {header_path}")
        sys.exit(1)
    raw = match.group(1)
    # Strip trailing line-continuation backslashes, then join lines
    # Each line in the Arduino source looks like: "base64data\n" \
    # We need to extract just the base64 content between quotes
    lines = raw.split('\n')
    pem_lines = []
    for line in lines:
        # Strip trailing backslash (line continuation)
        line = line.rstrip('\\').rstrip()
        # Remove surrounding quotes
        line = line.strip('"')
        # Remove trailing \n escape sequences (already handled by split)
        line = line.rstrip('\\n').rstrip()
        if line:
            pem_lines.append(line)
    return '\n'.join(pem_lines) + '\n'


def extract_string_macro(header_path: Path, macro_name: str) -> str:
    """Extract a string macro value from a header."""
    text = header_path.read_text()
    pattern = rf'#ifndef\s+{macro_name}\s*\n#define\s+{macro_name}\s+"(.*?)"'
    match = re.search(pattern, text)
    if not match:
        print(f"WARNING: Could not find {macro_name} in {header_path}, using placeholder")
        return "REPLACE_ME"
    return match.group(1)


def write_oms_ca(dest: Path, pem: str):
    """Write OMS CA cert as a const char[] array."""
    # Convert PEM to C string literal, line by line
    lines = pem.strip().split('\n')
    c_parts = []
    for line in lines:
        # Escape backslashes and quotes for C string literal
        escaped = line.replace('\\', '\\\\').replace('"', '\\"')
        c_parts.append(f'    "{escaped}\\n"')

    content = '/*\n'
    content += ' * OMS CA certificate for ESP32-C6 lock build.\n'
    content += ' * Auto-generated from src/security/oms_ca.h — do not edit manually.\n'
    content += ' * Run scripts/esp32c6-extract-secrets.py to update.\n'
    content += ' */\n\n'
    content += '#ifndef FORGEKEY_OMS_CA_H\n'
    content += '#define FORGEKEY_OMS_CA_H\n\n'
    content += 'static const char kOmsCaPem[] = {\n'
    content += ',\n'.join(c_parts) + '\n'
    content += '    NULL\n'
    content += '};\n\n'
    content += '#endif /* FORGEKEY_OMS_CA_H */\n'

    dest.mkdir(parents=True, exist_ok=True)
    (dest / "oms_ca.h").write_text(content)
    print(f"  Written: {dest / 'oms_ca.h'} ({len(pem)} bytes PEM)")


def write_firmware_pubkey(dest: Path, pem: str):
    """Write firmware pubkey as a const char[] array."""
    lines = pem.strip().split('\n')
    c_parts = []
    for line in lines:
        escaped = line.replace('\\', '\\\\').replace('"', '\\"')
        c_parts.append(f'    "{escaped}\\n"')

    content = '/*\n'
    content += ' * Firmware signing public key for ESP32-C6 lock build.\n'
    content += ' * Auto-generated from src/security/firmware_pubkey.h — do not edit manually.\n'
    content += ' * Run scripts/esp32c6-extract-secrets.py to update.\n'
    content += ' *\n'
    content += ' * NOTE: This is a PLACEHOLDER. Replace with the real key before production.\n'
    content += ' */\n\n'
    content += '#ifndef FORGEKEY_FIRMWARE_PUBKEY_H\n'
    content += '#define FORGEKEY_FIRMWARE_PUBKEY_H\n\n'
    content += 'static const char kFirmwarePubKeyPem[] = {\n'
    content += ',\n'.join(c_parts) + '\n'
    content += '    NULL\n'
    content += '};\n\n'
    content += '#endif /* FORGEKEY_FIRMWARE_PUBKEY_H */\n'

    dest.mkdir(parents=True, exist_ok=True)
    (dest / "firmware_pubkey.h").write_text(content)
    print(f"  Written: {dest / 'firmware_pubkey.h'} ({len(pem)} bytes PEM)")


def write_device_config(dest: Path, provisioning_token: str, jwt_secret: str):
    """Write device_config.h with extracted secrets."""
    content = f'''/*
 * Device configuration for ESP32-C6 lock build.
 * Auto-generated from Arduino project sources — do not edit manually.
 * Run scripts/esp32c6-extract-secrets.py to update.
 */

#ifndef FORGEKEY_DEVICE_CONFIG_H
#define FORGEKEY_DEVICE_CONFIG_H

/* OMS endpoint */
#ifndef OMS_HOST
#define OMS_HOST "dms.openmakersuite.net"
#endif

#ifndef OMS_PORT
#define OMS_PORT 443
#endif

/* Provisioning token (extracted from src/provisioning/device_config.h) */
#ifndef FORGEKEY_PROVISIONING_TOKEN
#define FORGEKEY_PROVISIONING_TOKEN "{provisioning_token}"
#endif

/* Sensor kind */
#ifndef FORGEKEY_SENSOR_KIND
#define FORGEKEY_SENSOR_KIND "cabinet-lock"
#endif

/* Firmware version */
#ifndef FORGEKEY_FIRMWARE_VERSION
#define FORGEKEY_FIRMWARE_VERSION "0.1.0"
#endif

/* MQTT broker fallback */
#ifndef MQTT_BROKER_FALLBACK_HOST
#define MQTT_BROKER_FALLBACK_HOST "dms.openmakersuite.net"
#endif

#ifndef MQTT_BROKER_FALLBACK_PORT
#define MQTT_BROKER_FALLBACK_PORT 8883
#endif

#ifndef MQTT_BROKER_FALLBACK_USE_TLS
#define MQTT_BROKER_FALLBACK_USE_TLS 1
#endif

/* Captive portal AP password */
#ifndef FORGEKEY_AP_PASSWORD
#define FORGEKEY_AP_PASSWORD "12345678"
#endif

#endif /* FORGEKEY_DEVICE_CONFIG_H */
'''
    dest.mkdir(parents=True, exist_ok=True)
    (dest / "device_config.h").write_text(content)
    token_preview = provisioning_token[:8] if len(provisioning_token) > 8 else provisioning_token
    token_display = f"{token_preview}...({len(provisioning_token)} chars)" if len(provisioning_token) > 8 else provisioning_token
    print(f"  Written: {dest / 'device_config.h'} (token: {token_display})")


def write_wifi_secrets(dest: Path, networks: list):
    """Write wifi_secrets.h with predefined networks if available."""
    if not networks:
        print("  No predefined WiFi networks found (secrets_local.h not present)")
        return

    lines = [
        '/*',
        ' * Predefined WiFi networks for development.',
        ' * Auto-generated from src/wifi_setup/secrets_local.h — do not edit manually.',
        ' * Run scripts/esp32c6-extract-secrets.py to update.',
        ' * Remove src/wifi_setup/secrets_local.h to disable predefined networks.',
        ' */',
        '',
        '#ifndef FORGEKEY_WIFI_SECRETS_H',
        '#define FORGEKEY_WIFI_SECRETS_H',
    ]

    for i, (ssid, password) in enumerate(networks, 1):
        lines.append(f'/* Network {i} */')
        lines.append(f'#define FORGEKEY_NETWORK_{i}_SSID "{ssid}"')
        lines.append(f'#define FORGEKEY_NETWORK_{i}_PASSWORD "{password}"')
        lines.append('')

    lines.extend([
        '#define FORGEKEY_HAS_PREDEFINED_NETWORKS 1',
        '',
        '#endif /* FORGEKEY_WIFI_SECRETS_H */',
    ])

    content = '\n'.join(lines) + '\n'
    dest.mkdir(parents=True, exist_ok=True)
    (dest / "wifi_secrets.h").write_text(content)
    print(f"  Written: {dest / 'wifi_secrets.h'} ({len(networks)} networks)")


def extract_wifi_networks():
    """Extract predefined WiFi networks from secrets_local.h if it exists."""
    secrets_path = SRC_WIFI / "secrets_local.h"
    if not secrets_path.exists():
        return []

    text = secrets_path.read_text()
    networks = []

    # Match FORGEKEY_NETWORK_N_SSID and FORGEKEY_NETWORK_N_PASSWORD pairs
    ssid_pattern = r'#define\s+FORGEKEY_NETWORK_(\d+)_SSID\s+"([^"]*)"'
    pass_pattern = r'#define\s+FORGEKEY_NETWORK_(\d+)_PASSWORD\s+"([^"]*)"'

    ssids = {int(m.group(1)): m.group(2) for m in re.finditer(ssid_pattern, text)}
    passwords = {int(m.group(1)): m.group(2) for m in re.finditer(pass_pattern, text)}

    for num in sorted(ssids.keys()):
        if num in passwords:
            networks.append((ssids[num], passwords[num]))

    return networks


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
    print("Extracting secrets from Arduino project for ESP32-C6 lock build...\n")

    # Extract provisioning token
    prov_token = extract_string_macro(SRC_PROVISIONING / "device_config.h", "FORGEKEY_PROVISIONING_TOKEN")
    print(f"  Provisioning token: {prov_token[:12]}..." if len(prov_token) > 12 else f"  Provisioning token: {prov_token}")

    # Extract JWT secret from lock config
    jwt_secret = extract_string_macro(SRC_LOCK / "lock_config.h", "FORGEKEY_LOCK_JWT_SECRET")
    print(f"  JWT secret: {jwt_secret[:12]}..." if len(jwt_secret) > 12 else f"  JWT secret: {jwt_secret}")

    # Extract PEM certificates
    oms_ca_pem = extract_pem_macro(SRC_SECURITY / "oms_ca.h", "OMS_CA_PEM")
    print(f"  OMS CA cert: {len(oms_ca_pem)} bytes")

    fw_pubkey_pem = extract_pem_macro(SRC_SECURITY / "firmware_pubkey.h", "FORGEKEY_FW_PUBKEY_PEM")
    print(f"  Firmware pubkey: {len(fw_pubkey_pem)} bytes (placeholder)")

    # Extract WiFi networks
    networks = extract_wifi_networks()
    if networks:
        print(f"  WiFi networks: {len(networks)} found")
    else:
        print("  WiFi networks: none (secrets_local.h not present)")

    # Write all outputs
    print("\nWriting files...")
    write_oms_ca(DEST, oms_ca_pem)
    write_firmware_pubkey(DEST, fw_pubkey_pem)
    write_device_config(DEST, prov_token, jwt_secret)
    write_wifi_secrets(DEST, networks)
    write_firmware_verify(DEST)
    write_firmware_verify_header(DEST)

    print("\nDone.")


if __name__ == "__main__":
    main()
