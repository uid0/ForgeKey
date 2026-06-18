# ForgeKey Verification Ladder

ForgeKey changes move through this ladder from cheapest host checks to hardware
smoke tests. Run the earliest applicable rung while iterating, then complete the
full required set before merging to `main`.

## Rung 0: host contract unit tests

Run on any developer machine; no ESP toolchain or hardware is required.

```bash
make install-dev
make test-host
```

Coverage:
- command parsing acceptance/rejection for supported command envelopes;
- OTA dispatch payload validation for HTTPS URLs, SHA-256 digests, and required
  signatures;
- ECDSA manifest signature verification and tamper rejection;
- WiFi/BLE desired-state reconciliation guardrails;
- schema registry validity and documentation coverage;
- telemetry payload JSON serialization against schema contracts.

## Rung 1: schema and documentation checks

```bash
make schemas
make docs-check
```

These checks validate JSON Schema syntax, enforce one documented
`schema_version` per schema file, and verify local Markdown links without relying
on network access.

## Rung 2: simulator contracts

```bash
make test-simulator
```

The simulator tests verify that each supported Wokwi-compatible variant has a
usable diagram and sketch:

| Variant | Wokwi project | What is asserted |
|---|---|---|
| Temperature sensor | `hardware/temperature-sensor/wokwi/` | DHT21/AM2301 simulation project is present and wired. |
| Cabinet lock | `hardware/cabinet-lock/wokwi/` | Lock input/output simulation project is present and wired. |

Wokwi simulation is intentionally a contract check: it catches missing diagrams,
sketches, and obvious harness drift. Firmware correctness still requires the
build and HIL rungs.

## Rung 3: firmware builds

```bash
make pio-build
```

Builds all PlatformIO environments, including the people-counter, temperature,
ePaper, and production variants declared in `platformio.ini`.

```bash
make lock-build
```

Builds the standalone ESP-IDF cabinet-lock firmware.

## Rung 4: hardware-in-the-loop smoke tests

Use real devices and capture a smoke evidence JSON file, then run:

```bash
make test-hil HIL_EVIDENCE=/path/to/smoke_evidence.json
```

The evidence file must mark each required smoke check as `pass`:

```json
{
  "checks": {
    "camera": {"status": "pass"},
    "dht21": {"status": "pass"},
    "epaper": {"status": "pass"},
    "lock_sensors": {"status": "pass"},
    "mqtt": {"status": "pass"},
    "wifi_provisioning": {"status": "pass"},
    "ota": {"status": "pass"}
  },
  "telemetry_samples": [
    {"schema_version": "forgekey.status.v1", "online": true}
  ]
}
```

Smoke procedure:
1. **Camera / people-counter:** flash the S3 people-counter build, connect WiFi,
   confirm camera init succeeds, and publish one schema-valid occupancy/status
   telemetry sample.
2. **DHT21:** flash the temperature build, apply a known room-temperature input,
   and confirm a schema-valid temperature telemetry sample.
3. **ePaper:** flash the ePaper build, fetch or stage a test render, refresh the
   display, and confirm the health sample reports render/display status.
4. **Lock sensors:** flash the ESP32-C6 lock, toggle closed/open and locked/unlocked
   inputs, actuate the output once, and confirm schema-valid lock status.
5. **MQTT:** verify TLS/mTLS connection, command subscription, telemetry publish,
   and retained state publish.
6. **WiFi provisioning:** erase provisioning state, complete captive-portal or
   development-secret provisioning, reboot, and confirm auto-reconnect.
7. **OTA:** dispatch a signed firmware manifest, verify download/apply status,
   reboot into the candidate image, and mark it stable.

## Required before committing to `main`

Before merging to `main`, the PR owner must provide evidence for:

```bash
make verify-host
make pio-build
make lock-build
make test-hil HIL_EVIDENCE=/path/to/smoke_evidence.json
```

HIL may be waived only for documentation-only changes that do not alter firmware,
schemas, hardware manifests, provisioning, MQTT topics, or OTA/security behavior.
If waived, record the reason in the PR body.
