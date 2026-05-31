# ForgeKey Hardware Artifacts

This directory is the repeatable source of truth for ForgeKey device-class hardware.
Each device-class directory contains the same artifact set so a build can be
reviewed, simulated where practical, and reproduced without tribal knowledge.

## Directory contract

Each device-class directory should include:

| File | Purpose |
|---|---|
| `README.md` | Human build overview and links to the artifacts below. |
| `pin-manifest.json` | Machine-readable board, pin, connector, and simulation metadata. |
| `wiring.svg` | Rendered wiring diagram committed as source-controlled SVG. |
| `bom.md` | Bill of materials with critical ratings/substitutions. |
| `harness-notes.md` | Connector, cable, strain-relief, labeling, and acceptance-test notes. |
| `simulation-limitations.md` | What Wokwi or other simulators can and cannot prove. |
| `wokwi/` | Wokwi `diagram.json` + firmware stub when the parts are simulator-supported. |

`future-modules/` is the template for new ForgeKey device classes. Copy it to a
new sibling directory, replace placeholder values, and keep the same filenames so
OMS/manufacturing tooling can discover artifacts consistently.

## Current simulation coverage

| Device class | Wokwi artifact | Notes |
|---|---|---|
| People counter | No | XIAO ESP32-S3 Sense camera/OV3660 + TFLite path is not represented by Wokwi. |
| Temperature sensor | Yes | Uses a Wokwi DHT22 model as protocol-compatible stand-in for DHT21/AM2301. |
| Cabinet lock | Yes | Simulates GPIO-level solenoid driver and switches; it does not energize a real lock. |
| ePaper display | No | The Seeed 7.5 in UC8179/XIAO ePaper driver board is not modeled. |

## Power and battery telemetry contract

All board manifests include a `battery_sense` object so firmware and OMS can
distinguish between a supported battery measurement and an intentionally
unsupported reference harness. Use `adc_pin: null` when no divider is fitted;
set `adc_pin` to an ADC-capable GPIO and `divider_ratio` to `R_total/R_low`
when the hardware divides the battery rail into the ESP32 ADC range. The
firmware reports wake reason, reset reason, brownout count, power source,
battery voltage/percent when configured, and low-battery/brownout alarms in
health telemetry.

Battery wiring options:

- USB/regulated-5V devices may leave `battery_sense.adc_pin` null; telemetry
  reports `power.battery.available=false` with an explicit unsupported reason.
- Battery-backed devices should sense the protected battery or backup rail
  through a high-value divider, keep the divided node below 3.3 V at maximum
  charge, share ground with the ESP32, and document the exact divider ratio in
  `pin-manifest.json`.
- Cabinet-lock solenoids still require an external lock PSU sized for coil
  inrush. If a backup battery is added, sense the logic/backup rail rather than
  the inductive solenoid node and preserve flyback protection.
