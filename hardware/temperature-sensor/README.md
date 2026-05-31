# Temperature-sensor Hardware

The temperature-sensor variant uses a Seeed XIAO ESP32-S3 plus a DHT21/AM2301
sensor. Firmware defaults the DHT data line to GPIO 2, exposed as D1 on the XIAO
header, and publishes temperature/humidity readings over the shared ForgeKey
MQTT/provisioning stack.

## Artifacts

- Machine-readable pin manifest: [`pin-manifest.json`](pin-manifest.json)
- Rendered wiring diagram: [`wiring.svg`](wiring.svg)
- BOM: [`bom.md`](bom.md)
- Connector and harness notes: [`harness-notes.md`](harness-notes.md)
- Wokwi simulation: [`wokwi/diagram.json`](wokwi/diagram.json) and [`wokwi/sketch.ino`](wokwi/sketch.ino)
- Simulation notes: [`simulation-limitations.md`](simulation-limitations.md)

## Critical wiring notes

- DHT21 VCC goes to 3V3, not 5 V, so the DATA high level remains safe for the
  ESP32-S3.
- Add a 4.7 kΩ to 10 kΩ pull-up from DATA to 3V3 for long harnesses.
- The Wokwi model uses DHT22 as a protocol-compatible stand-in for DHT21/AM2301.
