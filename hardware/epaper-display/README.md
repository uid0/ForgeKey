# ePaper-display Hardware

The preventive-maintenance ePaper display uses the Seeed XIAO 7.5 inch ePaper
Panel (SKU 6416) with a XIAO ESP32-C3 in the driver-board socket. The firmware
uses the Seeed driver-board pin map selected by `BOARD_SCREEN_COMBO=502` and
`USE_XIAO_EPAPER_DRIVER_BOARD`.

## Artifacts

- Machine-readable pin manifest: [`pin-manifest.json`](pin-manifest.json)
- Rendered wiring diagram: [`wiring.svg`](wiring.svg)
- BOM: [`bom.md`](bom.md)
- Connector and harness notes: [`harness-notes.md`](harness-notes.md)
- Simulation notes: [`simulation-limitations.md`](simulation-limitations.md)

## Critical wiring notes

- Use XIAO board labels, not raw GPIO numbers, when inspecting the Seeed driver
  board: RST D0, CS D1, BUSY D2, DC D3, SCK D8, MISO D9, MOSI D10.
- The board includes a LiPo charger and battery, but this SKU does not route a
  battery ADC line to the XIAO socket; stock firmware reports
  `battery.available=false` until a hardware revision or field divider mod adds
  an ADC sense path.
