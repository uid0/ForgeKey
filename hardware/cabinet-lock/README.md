# Cabinet-lock Hardware

The cabinet-lock build is the standalone ESP-IDF project under `esp32c6-lock/`.
It drives a solenoid through a low-side MOSFET and supervises door/latch state
with reed, IR beam, mortise, and latch-supervisor inputs.

## Artifacts

- Machine-readable pin manifest: [`pin-manifest.json`](pin-manifest.json)
- Rendered wiring diagram: [`wiring.svg`](wiring.svg)
- BOM: [`bom.md`](bom.md)
- Connector and harness notes: [`harness-notes.md`](harness-notes.md)
- Wokwi logic simulation: [`wokwi/diagram.json`](wokwi/diagram.json) and [`wokwi/sketch.c`](wokwi/sketch.c)
- Simulation notes: [`simulation-limitations.md`](simulation-limitations.md)

## Critical wiring notes

- The solenoid must never be driven directly from an ESP32 GPIO. Use a
  logic-level N-MOSFET low-side driver, gate resistor, gate pulldown, and a
  flyback diode across the coil.
- Reed, IR beam, mortise, and latch-supervisor inputs use internal pull-ups and
  active-low semantics in firmware.
- The 12 V lock supply negative and ESP32 ground must be common so the MOSFET
  gate drive has a valid reference.


## Power and battery options

The reference lock controller uses an external 12 V lock PSU for the solenoid
and a regulated 3.3 V logic rail for the ESP32-C6 and sensors. Battery backup is
optional and must not be wired directly to any GPIO. To report battery health,
add a resistor divider from the protected backup/logic battery rail to an
ADC-capable ESP32-C6 GPIO, keep the ADC node below 3.3 V at the highest charge
voltage, and set `battery_sense.adc_pin` plus `divider_ratio` in
`pin-manifest.json` and the matching ESP-IDF Kconfig values. Without that
divider, health telemetry explicitly reports battery sensing as unsupported.
