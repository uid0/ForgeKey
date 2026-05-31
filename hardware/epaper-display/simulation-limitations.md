# ePaper-display Simulation Limitations

No Wokwi project is provided because the simulator does not model the Seeed SKU
6416 UC8179 7.5 inch ePaper driver board, panel FPC, high-voltage ePaper drive
waveforms, deep-sleep battery behavior, or charger LEDs.

Use `wiring.svg` and `pin-manifest.json` for wiring review. Validate firmware on
real panels by confirming bind QR rendering, PNG fetch/decode, full refresh,
ETag 304 sleep behavior, and charger/battery swap procedure.
