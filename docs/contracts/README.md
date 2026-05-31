# ForgeKey Integration Contracts

This directory is the self-describing contract surface between ForgeKey firmware,
OMS, the MQTT broker, and HTTP upload/download services. Contract files are
versioned at the document and payload level so firmware and backend changes can
be rolled out independently.

## Contract index

| Area | Contract | Payload schemas |
|---|---|---|
| Provisioning enrollment and credential rotation | [`provisioning.v1.yaml`](provisioning.v1.yaml) | [`provisioning_enroll_request.v1.schema.json`](../schemas/provisioning_enroll_request.v1.schema.json), [`provisioning_enroll_response.v1.schema.json`](../schemas/provisioning_enroll_response.v1.schema.json) |
| Photo capture upload | [`photo-upload.v1.yaml`](photo-upload.v1.yaml) | [`photo_upload_metadata.v1.schema.json`](../schemas/photo_upload_metadata.v1.schema.json), [`photo_upload_result.v1.schema.json`](../schemas/photo_upload_result.v1.schema.json) |
| ePaper image delivery and battery telemetry | [`epaper.v1.yaml`](epaper.v1.yaml) | [`epaper_image_manifest.v1.schema.json`](../schemas/epaper_image_manifest.v1.schema.json), [`epaper_battery.v1.schema.json`](../schemas/epaper_battery.v1.schema.json), [`epaper.v1.schema.json`](../schemas/epaper.v1.schema.json) |
| OTA artifact delivery | [`ota-artifacts.v1.yaml`](ota-artifacts.v1.yaml) | [`ota_artifact_manifest.v1.schema.json`](../schemas/ota_artifact_manifest.v1.schema.json), [`ota_status.v1.schema.json`](../schemas/ota_status.v1.schema.json) |
| Diagnostics upload | [`diagnostics-upload.v1.yaml`](diagnostics-upload.v1.yaml) | [`diagnostics_upload.v1.schema.json`](../schemas/diagnostics_upload.v1.schema.json), [`diagnostics_upload_result.v1.schema.json`](../schemas/diagnostics_upload_result.v1.schema.json), [`diagnostics.v1.schema.json`](../schemas/diagnostics.v1.schema.json) |
| MQTT topics and payloads | [`mqtt.md`](mqtt.md) | [`command.v1.schema.json`](../schemas/command.v1.schema.json), [`command_ack.v1.schema.json`](../schemas/command_ack.v1.schema.json), telemetry schemas in [`../schemas/`](../schemas/) |
| Canonical error vocabulary | [`error-codes.md`](error-codes.md) | [`error.v1.schema.json`](../schemas/error.v1.schema.json) |

## Global compatibility rules

1. Every JSON object crossing a firmware/backend boundary MUST include a
   `schema_version` value unless the OpenAPI operation is a binary artifact
   response whose metadata is supplied by headers.
2. JSON Schema files live under [`docs/schemas/`](../schemas/) and are the only
   payload schemas referenced by the OpenAPI and MQTT contracts.
3. Unknown optional fields are allowed for additive releases. Required-field
   changes, enum removals, unit changes, or behavior changes require a new
   schema version and contract revision.
4. Producers MUST use canonical error codes from
   [`error-codes.md`](error-codes.md) and MAY include a human-readable `message`
   or operation-specific `details` object.
5. Firmware MUST reject unsupported safety-sensitive commands explicitly with a
   structured acknowledgement instead of silently ignoring them.
