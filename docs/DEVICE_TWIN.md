# ForgeKey Device Twin Contract

The device twin is the canonical contract between OMS and every ForgeKey ESP32
firmware target. OMS publishes the desired state; the device validates,
applies, persists, and reports the observed state. This gives operators one
place to see configuration drift, unsupported settings, firmware identity, and
last-known health.

## MQTT topics

| Topic | Direction | Retained | Purpose |
|-------|-----------|----------|---------|
| `forgekey/<device_id>/desired` | OMS → device | Yes | Versioned target configuration for the device. |
| `forgekey/<device_id>/reported` | Device → OMS | Yes | Last applied and observed state. |
| `forgekey/<device_id>/config_ack` | Device → OMS | No | Immediate acknowledgement for a desired-state revision. |
| `forgekey/<device_id>/health` | Device → OMS | No | Periodic runtime health report; see `DEVICE_HEALTH.md`. |

`<device_id>` should be the stable OMS device identity. Existing MAC-based
topics can remain as compatibility aliases until OMS has migrated.

## Desired-state envelope

```json
{
  "schema_version": "forgekey.device_desired.v1",
  "desired_revision": 42,
  "issued_at": "2026-05-31T00:00:00Z",
  "expires_at": "2026-06-30T00:00:00Z",
  "device_id": "fk_01HX...",
  "hardware_target": "seeed_xiao_esp32s3",
  "release_channel": "stable",
  "settings": {
    "telemetry_interval_s": 60,
    "log_level": "info",
    "identify_led": false,
    "ota": {
      "allow_remote_update": true,
      "minimum_version": "0.1.0",
      "cohort": "default"
    },
    "wifi": {
      "profiles_revision": 3
    },
    "capabilities": {
      "people_counter": {
        "enabled": true,
        "confidence_threshold": 0.65,
        "privacy_mask_revision": 1
      }
    }
  }
}
```

## Reported-state envelope

```json
{
  "schema_version": "forgekey.device_reported.v1",
  "reported_at": "2026-05-31T00:00:12Z",
  "device_id": "fk_01HX...",
  "mac": "aabbcc112233",
  "desired_revision": 42,
  "applied_revision": 42,
  "config_status": "applied",
  "unsupported": [],
  "firmware": {
    "version": "0.1.0",
    "build_id": "20260531.1",
    "git_sha": "unknown",
    "release_channel": "stable",
    "target": "seeed_xiao_esp32s3"
  },
  "hardware": {
    "board": "seeed_xiao_esp32s3",
    "chip": "esp32s3",
    "active_capabilities": ["people_counter", "status_led"]
  },
  "network": {
    "wifi_connected": true,
    "mqtt_connected": true,
    "ip": "192.168.1.50",
    "rssi": -57
  },
  "ota": {
    "slot": "ota_0",
    "pending_verify": false,
    "last_status": "idle"
  }
}
```

## Config acknowledgement

Devices should publish a `config_ack` as soon as a desired-state revision is
parsed. The acknowledgement must include the revision, whether the config was
accepted, and any rejected paths. Unsupported keys are not silent no-ops.

```json
{
  "schema_version": "forgekey.config_ack.v1",
  "device_id": "fk_01HX...",
  "desired_revision": 42,
  "accepted": false,
  "applied_revision": 41,
  "errors": [
    {
      "path": "settings.capabilities.people_counter.privacy_mask_revision",
      "code": "unsupported_setting",
      "message": "privacy masks are not implemented by this firmware"
    }
  ]
}
```

## Device-side apply rules

1. Validate `schema_version`, `device_id`, revision monotonicity, and expiry.
2. Reject settings for the wrong hardware target unless explicitly marked as
   cross-target.
3. Persist only settings that survived validation.
4. Apply settings in a safe order: networking changes must be tested before the
   previous known-good profile is deleted.
5. Publish `config_ack` immediately and `reported` after the setting has either
   taken effect or failed.
6. Keep the previous known-good state available for rollback after reboot.

## First firmware implementation slice

The first device-side implementation should support only these settings:

- `telemetry_interval_s`
- `log_level`
- `identify_led`
- `ota.allow_remote_update`
- `ota.minimum_version`
- capability `enabled` flags

Everything else should be acknowledged with `unsupported_setting` until the
corresponding firmware module exists.
