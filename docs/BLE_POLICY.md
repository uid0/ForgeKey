# ForgeKey BLE Privacy and Retention Policy

ForgeKey BLE capabilities are designed for production spaces where nearby phones,
wearables, laptops, and tags may advertise Bluetooth identifiers that do not
belong to ForgeKey. Firmware must treat those advertisements as sensitive
proximity metadata.

## Default privacy posture

- BLE scanner, beacon, relay, and equipment tracking are independently
  controlled by desired state; operators should enable only the capabilities
  required at a site.
- Raw third-party BLE MAC addresses are **not published by default**.
- Scan result payloads publish a derived `id` instead of `mac` unless
  `raw_mac_enabled` (or legacy `publish_raw_mac`) is explicitly set to `true`.
- Supported derived-ID modes are:
  - `hashed`: stable per `site_namespace` and BLE MAC, for recurring anonymous
    counts within a site.
  - `ephemeral`: rotates using a boot/hour salt so the same third-party device
    cannot be linked for long-term tracking by ForgeKey telemetry alone.
- `site_namespace` scopes hashes to a tenant/site. Moving a device to a new site
  must use a new namespace so hashed IDs cannot be correlated across sites.

## Runtime desired-state configuration

BLE desired state is accepted on `forgekey/<mac>/config` through either
`{"cmd":"set_ble", ...}` or a broader `{"cmd":"desired_state","ble":{...}}`
payload. The firmware persists the resolved BLE desired state in NVS and applies
it at boot before BLE capability setup.

Example production-safe payload:

```json
{
  "cmd": "set_ble",
  "command_id": "oms-123",
  "enabled": true,
  "scanner": true,
  "beacon": false,
  "relay": false,
  "equipment": true,
  "scan_interval_ms": 120000,
  "scan_duration_s": 5,
  "rssi_threshold": -82,
  "identity_mode": "ephemeral",
  "raw_mac_enabled": false,
  "site_namespace": "site-west-1",
  "allowlist": ["aa:bb:cc:11:22:33"],
  "denylist": ["de:ad:be:ef:00:01"]
}
```

Settings:

| Setting | Purpose | Production guidance |
| --- | --- | --- |
| `scanner` | Enables passive BLE advertisement scans. | Enable only where BLE metrics are required. |
| `beacon` | Enables ForgeKey BLE beacon advertising. | Disable where discoverability is not required. |
| `relay` | Enables inter-device BLE relay. | Enable only for approved offline relay deployments. |
| `equipment` | Enables configured equipment-tag tracking. | Prefer iBeacon UUID/tag configuration over raw MAC tags. |
| `scan_interval_ms` | Minimum interval between scan starts. | Use the longest interval that satisfies the use case. |
| `scan_duration_s` | Scan length, clamped to 1-30 seconds. | Keep short in occupied spaces. |
| `rssi_threshold` | Drops weak advertisements below this RSSI. | Raise threshold to reduce incidental collection. |
| `allowlist` | Optional list of accepted bare/colon MACs. | Use for approved tags; empty means no allowlist. |
| `denylist` | List of MACs always filtered. | Use for opt-out or sensitive devices. |
| `site_namespace` | Site/tenant hash namespace. | Unique per deployment space. |
| `raw_mac_enabled` | Allows publishing `mac`. | Off by default; use only with explicit approval. |
| `identity_mode` | `hashed` or `ephemeral`. | Prefer `ephemeral` for people-facing areas. |

## Data minimization and retention

- Firmware deduplicates advertisements within a short in-memory ring and does
  not persist third-party scan observations to NVS.
- BLE allowlists, denylists, equipment tags, and desired state are persisted
  because they are operator configuration, not observations.
- MQTT scan payloads should be retained by OMS only as long as needed for the
  approved operational purpose. Raw-MAC payloads require a shorter retention
  window and documented operator approval.
- Operators must not use BLE scan data to identify or profile people unless a
  separate legal basis and notice process exists outside ForgeKey firmware.

## Health and audit signals

Status snapshots include BLE desired state and per-capability health. BLE scanner
health reports scan count, filtered count, RSSI summary, last device count, and
last error so OMS can verify that privacy filters are active and spot unhealthy
scanners without exposing raw identifiers.
