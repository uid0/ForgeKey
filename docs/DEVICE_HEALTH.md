# ForgeKey Device Health Contract

Health telemetry is the periodic runtime snapshot used by OMS to detect failing
ESP32 devices before operators notice them. It is separate from command acks and
reported device-twin state: health is high-frequency operational telemetry,
while reported state is the retained configuration and identity snapshot.

## MQTT topic

`forgekey/<device_id>/health`

Health messages should be published at a configurable interval and after major
state transitions such as boot, provisioning, WiFi reconnect, MQTT reconnect,
OTA start, OTA completion, rollback, lock tamper, and capability failure.

## Health envelope

```json
{
  "schema_version": "forgekey.device_health.v1",
  "device_id": "fk_01HX...",
  "mac": "aabbcc112233",
  "reported_at": "2026-05-31T00:00:12Z",
  "uptime_ms": 421337,
  "severity": "ok",
  "firmware": {
    "version": "0.1.0",
    "build_id": "20260531.1",
    "git_sha": "unknown",
    "target": "seeed_xiao_esp32s3"
  },
  "boot": {
    "boot_count": 12,
    "reset_reason": "power_on",
    "brownout_count": 0,
    "last_crash_summary": null
  },
  "resources": {
    "free_heap": 187344,
    "min_free_heap": 160000,
    "psram_free": 1024000,
    "flash_size": 8388608
  },
  "network": {
    "wifi_connected": true,
    "mqtt_connected": true,
    "ip": "192.168.1.50",
    "rssi": -57,
    "wifi_reconnects": 1,
    "mqtt_reconnects": 0,
    "last_disconnect_reason": null,
    "time_synced": true,
    "last_time_sync_age_s": 12
  },
  "ota": {
    "state": "idle",
    "slot": "ota_0",
    "pending_verify": false,
    "last_version": "0.1.0",
    "last_error": null
  },
  "capabilities": {
    "people_counter": {
      "status": "ok",
      "last_tick_age_ms": 250,
      "last_error": null,
      "metrics": {
        "last_inference_ms": 91,
        "last_count": 3
      }
    },
    "status_led": {
      "status": "ok",
      "last_tick_age_ms": 250,
      "last_error": null
    }
  }
}
```

## Severity values

| Value | Meaning |
|-------|---------|
| `ok` | Device is operating normally. |
| `degraded` | Device is online but at least one capability or subsystem is impaired. |
| `critical` | Device is online but cannot perform its primary function safely. |
| `retired` | Device intentionally stopped normal service. |


## Hardware manifest health fields

Firmware includes a `hardware` object in status, firmware-status, and telemetry health payloads. The object reports the selected board manifest (`board_id`, `board_name`), GPIO ownership records, `active_capabilities`, and `skipped_capabilities`. Capability setup is manifest-gated at boot: firmware runs detection, validates GPIO ownership and unsafe/strap-pin constraints, then calls capability setup only for the remaining active capabilities.

## Capability health requirements

Every capability should eventually report:

- `status`: `ok`, `disabled`, `unsupported`, `degraded`, or `failed`.
- `last_tick_age_ms`: age of the last successful tick or work cycle.
- `last_error`: stable machine-readable error code or `null`.
- `metrics`: capability-specific counters and gauges.

Initial firmware work can publish capability health for active capabilities only;
future revisions should also report disabled and unsupported capabilities so OMS
can explain why desired state was not applied.

## Minimum first implementation

The first firmware implementation should add these fields to existing status or
new health publishing code:

- `schema_version`
- `device_id` or MAC compatibility identifier
- firmware version and target
- uptime
- free heap and minimum free heap where available
- reset reason
- WiFi connected, MQTT connected, IP, RSSI, and reconnect counters
- OTA state and pending verification state
- active capability list and one status value per active capability
