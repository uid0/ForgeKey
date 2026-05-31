# Future Module Hardware Template

Copy this directory to `hardware/<device-class>/` when adding a new ForgeKey
hardware class. Replace every placeholder before the module is considered ready
for firmware or OMS release.

## Required updates

- Fill `pin-manifest.json` with board identity, connectors, pins, power rails,
  protection components, and simulation status.
- Replace `wiring.svg` with a rendered wiring diagram generated from the final
  harness design.
- Complete `bom.md`, `harness-notes.md`, and `simulation-limitations.md`.
- Add `wokwi/` if the simulator supports enough of the hardware to prove GPIO
  polarity, protocol wiring, or firmware assumptions.
- Link the completed directory from the relevant top-level device document.
