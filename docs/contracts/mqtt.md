# ForgeKey MQTT Topic and Payload Contracts

This document is the canonical MQTT contract for firmware/backend integration.
All JSON MQTT payloads MUST carry `schema_version` and MUST validate against the
referenced JSON Schema under [`docs/schemas/`](../schemas/). Topic names use the
bare lowercase 12-hex MAC where legacy firmware still uses `<mac>`; newly
enrolled devices SHOULD also receive a stable OMS `device_id` in provisioning
and include it inside payloads when the schema allows additive fields.

## Common topic rules

| Rule | Requirement |
|---|---|
| Prefix | `forgekey/<mac>/...` unless OMS enrollment returns tenant-scoped aliases. |
| Authentication | MQTT over TLS with per-device credentials from provisioning; legacy username is `forgemqtt` with per-device JWT. |
| Commands | OMS publishes commands to `forgekey/<mac>/command`; device acks on `forgekey/<mac>/status`. |
| State/LWT | Device birth/LWT messages are retained on `forgekey/<mac>/state`. |
| QoS | QoS 1 preferred for commands, command acks, OTA status, and critical lock transitions; QoS 0 accepted for best-effort telemetry. |
| Retain | Only birth/LWT state and OMS-retained desired config/OTA assignments may be retained. Telemetry and acks are not retained. |
| Errors | Command and status payload errors use canonical codes from [`error-codes.md`](error-codes.md). |

## Shared topics for every device class

| Direction | Topic | Payload schema | Notes |
|---|---|---|---|
| OMS → device | `forgekey/<mac>/command` | [`forgekey.command.v1`](../schemas/command.v1.schema.json) | Signed command envelope with `cmd`, `command_id`, issue/expiry, nonce, actor, and authenticator. |
| Device → OMS | `forgekey/<mac>/status` | [`forgekey.command_ack.v1`](../schemas/command_ack.v1.schema.json), [`forgekey.status.v1`](../schemas/status.v1.schema.json), or class telemetry below | Command acks MUST include `command_id` when one was parsed. |
| Device/broker → OMS | `forgekey/<mac>/state` | [`forgekey.status.v1`](../schemas/status.v1.schema.json) | Retained birth/LWT online state. |
| Device → OMS | `forgekey/<mac>/health` | [`forgekey.health.v1`](../schemas/health.v1.schema.json) | Periodic health, memory, WiFi, clock, and subsystem status. |
| Device → OMS | `forgekey/<mac>/diagnostics` | [`forgekey.diagnostics.v1`](../schemas/diagnostics.v1.schema.json) | Low-volume log/fault events. Bulk bundles use the HTTPS diagnostics contract. |
| OMS → device | `forgekey/<mac>/ota` | [`forgekey.ota_artifact_manifest.v1`](../schemas/ota_artifact_manifest.v1.schema.json) or command envelope carrying an artifact reference | OTA assignments may be retained by OMS until superseded. |
| Device → OMS | `forgekey/<mac>/ota/status` | [`forgekey.ota_status.v1`](../schemas/ota_status.v1.schema.json) | Critical audit data; retry locally until MQTT client accepts publish. |
| Device → OMS | `forgekey/<mac>/ble` | [`forgekey.ble.v1`](../schemas/ble.v1.schema.json) | BLE observation summaries when the capability is enabled by policy. |

## Device-class topic matrix

| Device class | Topic | Direction | Payload schema | Required payload semantics |
|---|---|---|---|---|
| `people_counter` | `forgekey/<mac>/people_counter/occupancy` | Device → OMS | [`forgekey.occupancy.v1`](../schemas/occupancy.v1.schema.json) | `count` is the current occupancy estimate; `timestamp` is epoch seconds when clock is valid. |
| `people_counter` | `forgekey/<mac>/people_counter/photo` | Device → OMS | [`forgekey.photo_upload_result.v1`](../schemas/photo_upload_result.v1.schema.json) | MQTT publishes only upload result/receipt metadata; image bytes use [`photo-upload.v1.yaml`](photo-upload.v1.yaml). |
| `temperature_sensor` | `forgekey/<mac>/temperature_sensor/reading` | Device → OMS | [`forgekey.temperature.v1`](../schemas/temperature.v1.schema.json) | `tempC` is Celsius; humidity is optional for temperature-only hardware. |
| `cabinet_lock` | `forgekey/<mac>/lock/status` | Device → OMS | [`forgekey.lock_status.v1`](../schemas/lock_status.v1.schema.json) | `secure: true` is valid only when reed closed and latch locked; critical transitions SHOULD use QoS 1. |
| `cabinet_lock` | `forgekey/<mac>/lock/command` | OMS → device | [`forgekey.command.v1`](../schemas/command.v1.schema.json) | Alias for lock deployments that separate lock ACLs; same envelope and ack rules as the shared command topic. |
| `epaper_display` | `forgekey/<mac>/epaper/status` | Device → OMS | [`forgekey.epaper.v1`](../schemas/epaper.v1.schema.json) | Render/update state, current image ID, and display health. |
| `epaper_display` | `forgekey/<mac>/epaper/battery` | Device → OMS | [`forgekey.epaper_battery.v1`](../schemas/epaper_battery.v1.schema.json) | Battery voltage is millivolts; percentage is optional and bounded 0-100. |
| `indicator` | `forgekey/<mac>/indicator/status` | Device → OMS | [`forgekey.status.v1`](../schemas/status.v1.schema.json) | Reports current indicator state and capability metadata until a dedicated indicator schema is introduced. |
| `tool_controller` | `forgekey/<mac>/tool/status` | Device → OMS | [`forgekey.status.v1`](../schemas/status.v1.schema.json) | Reports enablement/metering state as additive status fields until a dedicated tool schema is introduced. |
| `accessory_controller` | `forgekey/<mac>/accessory/status` | Device → OMS | [`forgekey.status.v1`](../schemas/status.v1.schema.json) | Reports accessory relay/runout state as additive status fields until a dedicated accessory schema is introduced. |
| `power_relay` | `forgekey/<mac>/status` | Device → OMS | [`forgekey.status.v1`](../schemas/status.v1.schema.json) | Reports relay channel state and aggregate BL0942 voltage/current/power/energy under additive `power_relay` fields. |

## Command verbs by class

| Class | Required verbs | Optional verbs |
|---|---|---|
| All classes | `status`, `restart`, `identify`, `reprovision`, `retire` | `set_config`, `rotate_credentials`, `support_mode`, `run_diagnostics` |
| `people_counter` | `capture` | `set_privacy_mode`, `calibrate_counting_zone` |
| `temperature_sensor` | `sample` | `set_sample_interval`, `calibrate_sensor` |
| `cabinet_lock` | `unlock`, `lockout`, `clear_lockout`, `emergency_unlock` | `commission`, `init_ack` |
| `epaper_display` | `refresh` | `clear_screen`, `set_image`, `sleep` |
| `indicator` | `set_indicator` | `blink`, `set_pattern` |
| `tool_controller` | `enable`, `disable` | `set_metering_mode` |
| `accessory_controller` | `enable`, `disable` | `set_runout_timer` |
| `power_relay` | `power_set`, `relay_set` | `status` |

`indicator` devices accept `set_indicator` / `set_pattern`. The legacy form is a
single semantic keyword in `indicator` (or `state`); the extended form adds
explicit presentation overrides so OMS can drive arbitrary **color**,
**brightness**, and **pattern** on the WS2812 matrix. All new fields are
optional and additive; explicit `color` / `brightness` / `pattern` override the
keyword's defaults.

| Field | Type | Notes |
|---|---|---|
| `indicator` (or `state`) | string | Semantic keyword. Base: `ok`, `attention`, `error`, `busy`, `off`, `auto`. Aliases: `available`, `warning`, `critical`, `reserved`, `green`, `yellow`, `red`, `blue`, `clear`. New: `purple`/`magenta`, and status aliases `in_use`, `unavailable`, `classroom`/`class`, `locked_out`. The firmware maps each keyword to a default color/brightness/pattern. |
| `color` | string or array | Explicit color override. Named (`purple`, `magenta`, `green`, `red`, `blue`, `yellow`, `orange`, `pink`, `cyan`, `white`, `off`), hex `"#RRGGBB"`, or `[r,g,b]` (each 0-255). |
| `brightness` | string or int | `"low"` \| `"high"` \| integer 0-255. `low` ≈ 12% and `high` ≈ 90% of full scale, applied by scaling the color per-pixel (the global matrix brightness remains a master dimmer). |
| `pattern` | string | `"solid"` \| `"blink"` \| `"slow_blink"` \| `"breathe"` \| `"off"`. Default `solid`. |
| `period_ms` | int | Optional blink/breathe period. Defaults: blink 750, slow_blink 1500, breathe 2000. |
| `duration_s` | int | Auto-return the matrix to normal device status after N seconds. 0/missing = persist. |

Unknown color, brightness, or pattern values are rejected with
`error: "unsupported_indicator"`. The `command_ack.v1` for an accepted command
echoes `indicator` (when a keyword was sent), the resolved `color` (`#RRGGBB`),
`brightness` (int), and `pattern`. The firmware also reports current indicator
state on `forgekey/<mac>/indicator/status` using `forgekey.status.v1` additive
fields `indicator` / `color` / `brightness` / `pattern`.

Canonical presentation payloads OMS sends:

```json
{"cmd":"set_indicator","color":"green","brightness":"low","pattern":"solid"}   // available
{"cmd":"set_indicator","color":"green","brightness":"high","pattern":"solid"}  // in use
{"cmd":"set_indicator","color":"red","brightness":"low","pattern":"solid"}     // unavailable
{"cmd":"set_indicator","pattern":"off"}                                        // locked out
{"cmd":"set_indicator","color":"purple","brightness":"high","pattern":"slow_blink","period_ms":1500}  // in use for a class
```

Unsupported verbs MUST be acknowledged with `command_unsupported`. Authenticated
but unauthorized verbs MUST be acknowledged with `command_unauthorized`. Devices
MUST NOT silently ignore malformed or safety-sensitive commands.

## Example command acknowledgement

```json
{
  "schema_version": "forgekey.command_ack.v1",
  "cmd_ack": "unlock",
  "command_id": "cmd_01HV7J5H2G4R3M9K8N7P6Q5S4T",
  "ok": false,
  "error": "lock_lockout_active"
}
```

## Migration notes

Legacy topics documented in [`DEVICE_COMMANDS.md`](../../DEVICE_COMMANDS.md)
and [`LOCK_DEVICE.md`](../../LOCK_DEVICE.md) remain valid during migration. OMS
should prefer the explicit class topics above when provisioning new devices and
must continue accepting shared `forgekey/<mac>/status` telemetry from legacy
firmware until the migration window closes.
