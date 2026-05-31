# ForgeKey Schema Registry

This directory is reserved for machine-readable JSON Schemas that describe the
contracts between OMS and ForgeKey firmware. The first implementation pass is
only documentation; follow-up commits should add actual `.schema.json` files and
wire them into CI validation.

## Planned schemas

| Schema ID | File | Producer | Consumer |
|-----------|------|----------|----------|
| `forgekey.device_desired.v1` | `device_desired.v1.schema.json` | OMS | Device |
| `forgekey.device_reported.v1` | `device_reported.v1.schema.json` | Device | OMS |
| `forgekey.config_ack.v1` | `config_ack.v1.schema.json` | Device | OMS |
| `forgekey.device_health.v1` | `device_health.v1.schema.json` | Device | OMS |
| `forgekey.command.v1` | `command.v1.schema.json` | OMS | Device |
| `forgekey.command_ack.v1` | `command_ack.v1.schema.json` | Device | OMS |
| `forgekey.ota_status.v1` | `ota_status.v1.schema.json` | Device | OMS |
| `forgekey.people_counter.v1` | `people_counter.v1.schema.json` | Device | OMS |
| `forgekey.temperature.v1` | `temperature.v1.schema.json` | Device | OMS |
| `forgekey.cabinet_lock.v1` | `cabinet_lock.v1.schema.json` | Device | OMS |
| `forgekey.epaper_display.v1` | `epaper_display.v1.schema.json` | Device | OMS |
| `forgekey.ble_observation.v1` | `ble_observation.v1.schema.json` | Device | OMS |
| `forgekey.diagnostics.v1` | `diagnostics.v1.schema.json` | Device | OMS |

## Compatibility rules

1. Schema IDs are immutable once implemented.
2. Additive optional fields can remain in the same schema version.
3. Required-field changes, enum removals, semantic changes, or unit changes
   require a new schema version.
4. Firmware should ignore unknown optional fields only after validating the
   envelope and revision.
5. Unsupported desired-state fields must be acknowledged explicitly instead of
   silently ignored.
6. Example payloads in documentation should be validated against these schemas
   once CI support exists.
