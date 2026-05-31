# People-counter Hardware

The people-counter build uses the Seeed XIAO ESP32-S3 Sense with its integrated
OV3660 camera module. The firmware pin assignments are mirrored in
`pin-manifest.json` and should match `src/camera/camera_config.h` plus the board
manifest emitted in device health.

## Artifacts

- Machine-readable pin manifest: [`pin-manifest.json`](pin-manifest.json)
- Rendered wiring diagram: [`wiring.svg`](wiring.svg)
- BOM: [`bom.md`](bom.md)
- Connector and harness notes: [`harness-notes.md`](harness-notes.md)
- Simulation notes: [`simulation-limitations.md`](simulation-limitations.md)

## Critical wiring notes

- Treat the camera as a factory mezzanine/FPC assembly; do not hand-wire the
  OV3660 parallel bus unless a new carrier board is being designed.
- Camera signals include XCLK, PCLK, VSYNC, HREF, D0-D7, and SCCB SDA/SCL.
- Power can be USB-C during development or a regulated 5 V feed in fixtures.
  Keep the camera, MCU, and any fixture supply on a common ground.
