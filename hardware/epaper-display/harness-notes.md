# ePaper-display Connector and Harness Notes

- **XIAO socket:** the driver board maps the panel control lines to XIAO D-labels
  D0, D1, D2, D3, D8, D9, and D10. Do not transpose to another XIAO module's raw
  GPIO numbering when reviewing hardware.
- **Panel FPC:** leave the factory FPC fully seated; avoid sharp bends and do not
  route the battery lead over the glass edge.
- **Battery:** use only protected LiPo packs compatible with the onboard charger.
  The charger status LEDs are the field-visible battery indicator.
- **Acceptance check:** charge the panel, boot once, verify bind QR rendering,
  bind to an asset in OMS, then verify the next wake paints the asset PNG.
