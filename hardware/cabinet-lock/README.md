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
