# People-counter Connector and Harness Notes

- **Camera:** leave the factory camera FPC/mezzanine seated and locked. Inspect
  the connector latch before closing the enclosure.
- **Power:** USB-C is preferred for prototypes. For wired installations, feed a
  regulated 5 V rail and GND to the XIAO power pads only if the enclosure has
  strain relief.
- **Grounding:** external LEDs, test jigs, and power supplies must share ground
  with the XIAO.
- **Acceptance check:** boot the firmware, confirm camera initialization in the
  serial log, capture a JPEG upload, then verify the OMS device photo updates.
- **Labeling:** label enclosure with device MAC, claim code, firmware variant
  `people-counter`, and camera orientation arrow.
