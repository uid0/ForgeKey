# ForgeKey Schema Registry

Machine-readable JSON Schemas in this directory define the contract between
ForgeKey firmware and OMS. Firmware-published payloads now carry a
`schema_version` field so OMS can route, validate, and migrate consumers without
inferring payload shape only from MQTT topic names or HTTP endpoints.

## Implemented schemas

| Schema version | File | Producer | Consumer |
|---|---|---|---|
| `forgekey.status.v1` | `status.v1.schema.json` | Device | OMS |
| `forgekey.health.v1` | `health.v1.schema.json` | Device | OMS |
| `forgekey.command.v1` | `command.v1.schema.json` | OMS | Device |
| `forgekey.command_ack.v1` | `command_ack.v1.schema.json` | Device | OMS |
| `forgekey.ota_status.v1` | `ota_status.v1.schema.json` | Device | OMS |
| `forgekey.occupancy.v1` | `occupancy.v1.schema.json` | Device | OMS |
| `forgekey.temperature.v1` | `temperature.v1.schema.json` | Device | OMS |
| `forgekey.lock_status.v1` | `lock_status.v1.schema.json` | Device | OMS |
| `forgekey.epaper.v1` | `epaper.v1.schema.json` | Device | OMS |
| `forgekey.ble.v1` | `ble.v1.schema.json` | Device | OMS |
| `forgekey.diagnostics.v1` | `diagnostics.v1.schema.json` | Device | OMS |
| `forgekey.error.v1` | `error.v1.schema.json` | Device/OMS | Device/OMS |
| `forgekey.provisioning_enroll_request.v1` | `provisioning_enroll_request.v1.schema.json` | Device | OMS |
| `forgekey.provisioning_enroll_response.v1` | `provisioning_enroll_response.v1.schema.json` | OMS | Device |
| `forgekey.photo_upload_metadata.v1` | `photo_upload_metadata.v1.schema.json` | Device | OMS |
| `forgekey.photo_upload_result.v1` | `photo_upload_result.v1.schema.json` | OMS | Device |
| `forgekey.epaper_image_manifest.v1` | `epaper_image_manifest.v1.schema.json` | OMS | Device |
| `forgekey.epaper_battery.v1` | `epaper_battery.v1.schema.json` | Device | OMS |
| `forgekey.ota_artifact_manifest.v1` | `ota_artifact_manifest.v1.schema.json` | OMS | Device |
| `forgekey.diagnostics_upload.v1` | `diagnostics_upload.v1.schema.json` | Device | OMS |
| `forgekey.diagnostics_upload_result.v1` | `diagnostics_upload_result.v1.schema.json` | OMS | Device |
| `forgekey.access_request.v1` | `access_request.v1.schema.json` | Device | OMS |

## Contract documents

- [`../contracts/README.md`](../contracts/README.md) indexes the HTTP OpenAPI, MQTT, and error-code contracts that reference these schemas.
- [`../contracts/mqtt.md`](../contracts/mqtt.md) maps each device class topic to the schema used on that topic.
- [`../contracts/error-codes.md`](../contracts/error-codes.md) defines the canonical error-code enum used by `error.v1.schema.json` and acknowledgement payloads.

## Compatibility rules

1. Schema IDs are immutable once implemented.
2. Additive optional fields can remain in the same schema version.
3. Required-field changes, enum removals, semantic changes, or unit changes
   require a new schema version.
4. OMS consumers must accept unknown optional fields after validating the
   `schema_version` envelope.
5. During the migration window, a missing `schema_version` identifies legacy v0
   firmware and should be handled explicitly by OMS consumers.
6. Unsupported desired-state or command fields must be acknowledged explicitly
   instead of silently ignored.
