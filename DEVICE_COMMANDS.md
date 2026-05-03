# ForgeKey Device Commands

Per-device control plane between OMS and ForgeKey firmware. OMS publishes
JSON commands on `forgekey/<mac>/command`; the device replies with structured
acks on `forgekey/<mac>/status`.

`<mac>` is the bare lowercase 12-hex MAC (no separators), e.g.
`aabbcc112233`.

## Topic shape

| Direction | Topic | Producer | Consumer |
|-----------|-------|----------|----------|
| Command   | `forgekey/<mac>/command` | OMS | Device |
| Ack/state | `forgekey/<mac>/status`  | Device | OMS |

Both use QoS 0, retain=false. The MQTT username is `forgemqtt`; the password
is the per-device JWT issued at registration.

## Common ack shape

Every command replies on the status topic with:

```json
{ "cmd_ack": "<original cmd>", ...command-specific fields... }
```

A second ack may follow for asynchronous commands (e.g. `capture` reports
both `queued:true` and a later `upload_status`).

Unknown commands receive:

```json
{ "cmd_ack": "<original cmd>", "error": "unknown_command" }
```

Silent drops are explicitly avoided — an OMS UI seeing no ack indicates a
broker/network problem, not a malformed command.

## Commands

### `restart`

```json
{ "cmd": "restart" }
```

Ack:

```json
{ "cmd_ack": "restart", "in_ms": 1000 }
```

The device flushes the ack to the broker and reboots ~1s later via
`ESP.restart()`. No further ack — OMS infers reboot from the device
re-publishing its capabilities announcement on next boot.

### `capture` (alias: `capture_photo`)

```json
{ "cmd": "capture" }
```

People-counter builds queue a one-shot photo capture + upload at the next
loop iteration (gated by the same post-inference quiet window as the
periodic uploader, so TLS doesn't tear down mid-handshake).

Initial ack:

```json
{ "cmd_ack": "capture", "queued": true }
```

Final ack (after upload completes or fails):

```json
{ "cmd_ack": "capture", "upload_status": "ok" }
{ "cmd_ack": "capture", "upload_status": "failed", "reason": "transport_error" }
```

`reason` enumeration:
- `auth_expired` — JWT rejected (401); device clears creds for re-register
- `server_error` — OMS returned 5xx
- `transport_error` — TLS / socket failure
- `bad_response` — non-2xx, non-401, non-5xx
- `capture_failed` — camera returned no frame

Builds without a camera (e.g. `seeed_xiao_esp32s3_temperature`) reject the
command up front:

```json
{ "cmd_ack": "capture", "queued": false, "error": "capability_unsupported" }
```

People-counter builds where the camera failed to init at boot:

```json
{ "cmd_ack": "capture", "queued": false, "error": "camera_unavailable" }
```

### `status` / `ping`

```json
{ "cmd": "status" }
{ "cmd": "ping" }
```

Both publish a snapshot on the status topic without changing device state.
Useful for "is the device alive" polling without relying on its periodic
publishes.

```json
{
  "cmd_ack": "status",
  "capabilities": ["people_counter", "status_led"],
  "firmware_version": "0.1.0",
  "free_heap": 187344,
  "rssi": -57,
  "uptime_ms": 421337,
  "mac": "aabbcc112233"
}
```

### `identify`

Operator wants to physically locate the device. Same as `blink:start` but
auto-clears after a configurable duration.

```json
{ "cmd": "identify" }
{ "cmd": "identify", "duration_s": 60 }
```

Default `duration_s` is 30 (override via `-DIDENTIFY_DEFAULT_DURATION_S=N`
at build time).

Acks:

```json
{ "blink": "on" }
{ "cmd_ack": "identify", "duration_s": 30 }
```

When the timer expires, the device echoes:

```json
{ "blink": "off" }
```

### `blink`

Operator-toggled identify LED with no auto-clear.

```json
{ "cmd": "blink" }                       // toggle
{ "cmd": "blink", "on": true }
{ "cmd": "blink", "on": false }
{ "cmd": "blink", "action": "start" }
{ "cmd": "blink", "action": "stop" }
{ "cmd": "blink", "action": "toggle" }
```

Echo on transition:

```json
{ "blink": "on" }
{ "blink": "off" }
```

No echo on no-op (already in requested state).

## Out of scope

- OTA dispatch — separate topic (`forgekey/<mac>/<kind>/firmware`), see
  `docs/ota/` for the dispatch payload schema.
- Capability-specific commands (e.g. people-counter zone reconfiguration) —
  reserved for future per-capability subtopics like
  `forgekey/<mac>/people_counter/command`.
