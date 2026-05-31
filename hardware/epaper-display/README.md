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
  `power.battery.available=false` until a hardware revision or field divider mod adds
  an ADC sense path.


## Power and battery options

The stock Seeed SKU 6416 includes a LiPo charger and 2000 mAh pack, but the
battery rail is not exposed to the XIAO socket as an ADC-safe sense line. The
reference manifest therefore leaves `battery_sense.adc_pin` null and firmware
reports `power.battery.available=false` with reason
`battery_adc_not_configured`. A field modification or future carrier revision
can add a high-value divider from BAT_4V2 to an ADC-capable XIAO pin; document
the chosen GPIO and divider ratio in `pin-manifest.json` and compile the
matching battery ADC macros before relying on voltage/percent telemetry.
