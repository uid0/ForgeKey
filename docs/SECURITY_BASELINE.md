# ForgeKey Production Security Baseline

This baseline applies to production ForgeKey people-counter, temperature,
ePaper, and ESP32-C6 cabinet-lock devices. Development devices may use the
non-production build profiles, but any device installed in the field must be
manufactured with the production profile and recorded in OMS before handoff.

## Build profiles and immutable controls

| Firmware | Development build | Production build | Security controls |
| --- | --- | --- | --- |
| People counter | `pio run -e seeed_xiao_esp32s3` | `pio run -e seeed_xiao_esp32s3_prod` | Secure boot/flash-encryption sdkconfig defaults where supported, encrypted NVS partition table, production NVS routing. |
| Temperature sensor | `pio run -e seeed_xiao_esp32s3_temperature` | `pio run -e seeed_xiao_esp32s3_temperature_prod` | Same as people counter. |
| ePaper display | `pio run -e seeed_xiao_epaper` | `pio run -e seeed_xiao_epaper_prod` | Same as people counter; verify target support before burning eFuses. |
| Cabinet lock | `cd esp32c6-lock && idf.py build` | `cd esp32c6-lock && idf.py build` using the checked-in production defaults | ESP32-C6 secure boot v2, flash encryption release mode, encrypted NVS, locked debug policy. |

Production firmware uses `partitions/forgekey_secure_ota.csv` or
`esp32c6-lock/partitions_prod.csv`. Both mark NVS and user-data partitions as
encrypted and include an `nvs_keys` partition for ESP-IDF NVS encryption.

> **One-way warning:** Secure boot, flash encryption release mode, JTAG disable,
and UART download restrictions burn eFuses. Treat production flashing as an
> irreversible manufacturing action. Never enable these controls on boards that
> must remain usable for firmware debugging.

## Secure boot baseline

1. Firmware signing keys are generated and stored offline as described in
   `docs/OTA_DEPLOYMENT.md`.
2. Production images must be signed before release. The public verification key
   embedded in firmware must match the private key configured in OMS.
3. Manufacturing must flash the bootloader, partition table, app image, and any
   required secure-boot digest/signature artifacts in one controlled session.
4. After first verified boot, record in the manufacturing log that secure boot is
   enabled and that unsigned images are rejected.
5. OTA releases must preserve the same secure-boot trust chain until the
   documented firmware-signing key-rotation process has completed.

## Flash encryption baseline

1. Production builds enable flash encryption in release mode where the ESP32
   target and framework support it.
2. Flash encryption keys are generated per device by the chip/eFuse flow and are
   never exported into OMS, CI, or operator laptops.
3. The encrypted partition table is mandatory for production OTA-capable devices.
4. Factory technicians must perform a post-flash reboot and verify that the
   device still reaches WiFi/provisioning after encryption is active.
5. Do not ship a device if the generated sdkconfig, boot log, or `espefuse.py`
   summary shows development flash-encryption mode.

## NVS encryption and sensitive storage

Sensitive values must be stored only in encrypted NVS on production devices:

- Per-device MQTT client certificate and private key.
- MQTT broker host/port/TLS policy returned by OMS enrollment.
- Firmware and telemetry MQTT topics returned by OMS enrollment.
- OMS command public-key cache.
- Rotated provisioning token (`prov_tok`) and other provisioning artifacts.
- Asset/display identifiers that bind the device to an OMS record.

The Arduino firmware compiles production profiles with
`FORGEKEY_PRODUCTION_SECURITY` and `FORGEKEY_SECURE_NVS_PARTITION="nvs"`, so the
provisioning namespace is opened from the encrypted production NVS partition.
The ESP32-C6 lock firmware enables `CONFIG_FORGEKEY_PRODUCTION_SECURITY` and
uses the same encrypted NVS partition for the `forgekey` namespace.

Non-sensitive operational queues may remain in their existing namespaces, but no
new secret-bearing value may be added outside the encrypted `forgekey` namespace
without updating this baseline.

## JTAG and UART policy

- **Production:** JTAG is disabled. UART console output is disabled or reduced to
  bootloader/application warnings, and UART download-mode encryption/decryption
  helpers are not allowed.
- **Manufacturing:** Before eFuse lock-down, UART may be used only on the
  manufacturing workstation to flash and verify the device. Workstations must not
  store production credentials after the device is accepted.
- **Development/RMA:** Use development builds on lab-only boards. If an RMA unit
  needs invasive debug, retire its production identity first and issue a new OMS
  identity only after it is re-manufactured.

## Manufacturing procedure: per-device credentials and OMS identity

1. **Prepare the station**
   - Sync the approved release commit.
   - Confirm the expected signing-key ID and OMS environment.
   - Build the correct production profile.
   - Attach the device over the manufacturing USB/UART jig.
2. **Flash production firmware**
   - Flash bootloader, partition table, app image, OTA metadata, and secure-boot
     artifacts produced by the build.
   - Allow first boot to complete secure-boot and flash-encryption provisioning.
   - Reboot once more and verify the production image still boots.
3. **Inject or derive per-device credentials**
   - Let firmware generate the device keypair and CSR on-device during OMS
     enrollment whenever possible.
   - If a contract manufacturer must inject credentials offline, use an encrypted
     NVS image generated for exactly one serial/MAC address, write it once, and
     destroy the workstation copy after OMS acceptance.
   - Never reuse MQTT certificates, private keys, provisioning tokens, or
     bootstrap artifacts between devices.
4. **Enroll with OMS**
   - The device submits MAC address, chip ID, flash ID, firmware version, sensor
     kind, CSR, and optional boot photo/proof data to OMS.
   - OMS issues the per-device client certificate, MQTT policy, command public
     key, and asset/display binding data.
5. **Record public identity in OMS**
   - Record device ID, MAC address, unique chip ID, flash ID, hardware variant,
     firmware version, signing-key ID, manufacturing batch, and public
     certificate fingerprint.
   - Do **not** record private keys, flash-encryption keys, raw provisioning
     tokens, or workstation temporary files.
6. **Acceptance checks**
   - Device connects to MQTT over TLS with the issued client certificate.
   - Device publishes a health/status message under its OMS-assigned topic.
   - OTA subscription uses the OMS-assigned firmware topic.
   - OMS inventory shows the expected public identity and no duplicate MAC/chip
     ID/certificate fingerprint.

## Routine key rotation

- Rotate MQTT credentials through OMS by issuing a new certificate/policy,
  delivering it over the authenticated configuration channel, and confirming the
  device reconnects under the new certificate before revoking the old one.
- Rotate OMS command keys by deploying firmware or a signed config payload that
  carries the new public key while OMS temporarily signs commands with both the
  old and new private keys.
- Rotate CA bundles through a bridge firmware that trusts both old and new CA
  roots, then remove the old root after all reachable devices report the bridge
  version or later.
- Rotate firmware signing keys with a bridge image containing the next public
  key. Do not sign normal releases only with the new key until the bridge rollout
  is complete and verified.

## Emergency key-rotation process

Use this process when a signing key, command key, MQTT CA/client credential, or
CA bundle is suspected to be compromised.

1. **Declare incident and freeze releases**
   - Assign an incident commander.
   - Stop non-emergency OTA rollouts and manufacturing.
   - Snapshot OMS inventory, MQTT ACLs, signing-key IDs, CA bundle versions, and
     firmware versions.
2. **Contain by key class**
   - **Firmware signing key:** Remove the compromised private key from OMS/CI,
     generate a replacement offline, build a bridge firmware that trusts the new
     public key, and sign emergency artifacts according to the last trusted key
     that deployed devices accept.
   - **OMS command key:** Disable command issuance with the compromised private
     key, publish a signed config/firmware update containing the new command
     public key, then reject commands signed by the old key after the migration
     window.
   - **MQTT credentials:** Revoke affected client certificates or username/token
     material in the broker, issue replacements per device, and force reconnects
     after devices acknowledge the new credentials.
   - **CA bundle/root:** Build a bridge firmware with both old and new roots if
     the old root is still needed for reachability; otherwise ship an emergency
     image pinned only to the new root through the remaining trusted path.
3. **Prioritize devices**
   - Rotate locks and safety-sensitive devices first.
   - Rotate devices with recent health check-ins before dormant devices.
   - Quarantine devices that fail rotation or present duplicate identity.
4. **Verify**
   - Confirm OTA signature verification, command validation, MQTT TLS auth, and
     HTTPS enrollment/OTA endpoints against the new trust material.
   - Confirm OMS shows the new key IDs/certificate fingerprints and no traffic
     using revoked credentials.
5. **Revoke and clean up**
   - Revoke old public identities, broker ACLs, command-key IDs, and CA bundle
     versions after the migration window.
   - Archive incident artifacts, hashes, and operator actions.
   - Update this document and `docs/OTA_DEPLOYMENT.md` with any lessons learned.

## Device retirement

1. Mark the device retired in OMS and remove it from active cohorts.
2. Revoke MQTT credentials, provisioning identity, command authorization, and OTA
   eligibility.
3. If physically available, run factory erase or destroy flash/eFuse-bearing
   modules according to data-handling requirements.
4. Remove labels that expose device identifiers before disposal or reuse.
5. Reuse hardware only by sending it through the full manufacturing procedure as
   a new OMS identity; never re-enable a retired production identity.
