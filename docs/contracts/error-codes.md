# ForgeKey Canonical Error Codes

All firmware/backend contracts use the `forgekey.error.v1` shape for machine
readable failures: `schema_version`, `code`, optional `message`, optional
`correlation_id`, optional `command_id`, and optional `details`. HTTP responses
SHOULD use the same `code` in the JSON body and in the `X-ForgeKey-Error-Code`
header when practical. MQTT acknowledgements use the same values in their
`error` field.

## Code format

Error codes are lowercase snake_case strings. New codes must be additive and
must be documented here before firmware or OMS emits them.

## Provisioning

| Code | Meaning | Typical action |
|---|---|---|
| `provisioning_token_missing` | Enrollment request has no bootstrap token or proof. | Re-run factory provisioning or issue a new bootstrap token. |
| `provisioning_token_invalid` | Token/proof cannot be verified. | Reject enrollment and audit the serial/MAC. |
| `provisioning_token_expired` | Token is valid but outside its enrollment window. | Issue a fresh token from OMS. |
| `provisioning_device_unknown` | Serial, MAC, or hardware ID is not registered for the tenant. | Add the device to inventory before retrying. |
| `provisioning_device_revoked` | Device identity was revoked or retired. | Do not reconnect; quarantine the unit. |
| `provisioning_csr_invalid` | CSR is malformed or does not match the device identity. | Regenerate device keypair/CSR. |
| `provisioning_policy_denied` | Tenant/site policy does not allow the requested class or capability. | Fix OMS assignment. |
| `provisioning_rate_limited` | Too many enrollment attempts. | Back off with jitter. |

## MQTT and command processing

| Code | Meaning | Typical action |
|---|---|---|
| `mqtt_not_configured` | Broker credentials or topics are missing. | Re-enroll or rotate credentials. |
| `mqtt_auth_failed` | Broker rejected the client certificate, username, or token. | Rotate credentials and inspect revocation status. |
| `mqtt_acl_denied` | Client attempted a topic outside its policy. | Correct OMS topic policy. |
| `mqtt_publish_failed` | Local client could not accept/publish an outbound message. | Queue and retry with backoff. |
| `mqtt_payload_invalid` | Payload is not valid JSON or does not match the declared schema. | Drop/reject and emit an ack where possible. |
| `command_parse_error` | Command JSON could not be parsed. | Fix OMS command serialization. |
| `command_missing_field` | Required command envelope field is absent. | Include the required field and retry with a new nonce. |
| `command_expired` | `expires_at` is in the past. | Reissue the command. |
| `command_replay` | `command_id` or nonce was already accepted or rejected. | Treat the retry as complete; do not act twice. |
| `command_signature_invalid` | JWT or detached signature failed verification. | Reject and audit operator/session. |
| `command_unauthorized` | Authenticated actor lacks permission for the command. | Escalate authorization in OMS. |
| `command_unsupported` | Device class/capability does not implement the verb. | Hide the command for this capability. |
| `command_busy` | Device is already performing an incompatible operation. | Retry after acked operation completes. |

## OTA

| Code | Meaning | Typical action |
|---|---|---|
| `ota_manifest_invalid` | OTA manifest is malformed or fails schema validation. | Regenerate artifact metadata. |
| `ota_artifact_not_found` | Requested artifact/version is unavailable. | Remove assignment or upload artifact. |
| `ota_version_incompatible` | Artifact target does not match hardware, device class, or partition policy. | Assign the correct build. |
| `ota_signature_invalid` | Artifact signature or signing-key ID is invalid. | Stop rollout and inspect signing pipeline. |
| `ota_sha256_mismatch` | Downloaded bytes do not match the manifest digest. | Re-download; quarantine CDN/object if repeated. |
| `ota_download_failed` | HTTP/TLS transfer failed before a complete artifact was received. | Retry with backoff. |
| `ota_flash_write_failed` | Firmware could not write the update partition. | Retry once, then service the device. |
| `ota_rollback` | Boot validation failed and firmware rolled back. | Mark rollout failed and keep previous image. |

## Diagnostics, locks, and sensors

| Code | Meaning | Typical action |
|---|---|---|
| `diagnostics_buffer_overflow` | Device dropped diagnostic records before upload. | Increase cadence or buffer size. |
| `diagnostics_upload_failed` | Diagnostics upload failed after retries. | Retry later and inspect connectivity. |
| `diagnostics_payload_too_large` | Upload exceeds OMS limit. | Split bundles or lower log verbosity. |
| `lock_invalid_token` | Unlock/lock operation token is invalid. | Reject operation and audit access attempt. |
| `lock_forced_open` | Reed/latch state indicates unauthorized opening. | Raise alarm and notify OMS. |
| `lock_jammed` | Latch/solenoid did not reach the expected state. | Service cabinet hardware. |
| `lock_sensor_conflict` | Reed, latch, mortise, or beam readings are physically inconsistent. | Inspect wiring/sensors. |
| `lock_lockout_active` | Lockout prevents normal unlock. | Clear lockout with an authorized command. |
| `sensor_unavailable` | Sensor is absent, uninitialized, or failed self-test. | Reinitialize or mark capability degraded. |
| `sensor_read_timeout` | Sensor did not provide a sample before timeout. | Retry and watch health telemetry. |
| `sensor_calibration_required` | Reading is blocked until calibration/config is supplied. | Apply calibration in desired state. |
| `sensor_value_out_of_range` | Reading is outside valid physical or configured limits. | Reject sample and inspect sensor. |
