# ForgeKey Installation and QR Onboarding Checklists

This guide standardizes field installation across active hardware variants. Use
it with the operator runbook and the device-class docs.

## QR-code onboarding format

Print one QR label per physical device or mounting location. The QR payload is a
UTF-8 JSON object so it can be scanned by OMS, a phone camera, or a plain QR
reader.

```json
{
  "schema_version": "forgekey.onboarding_qr.v1",
  "device_class": "people_counter",
  "asset_tag": "FK-PC-001",
  "install_location": "woodshop south door",
  "expected_build_target": "seeed_xiao_esp32s3",
  "oms_claim_url": "https://oms.example.org/forgekey/claim/FK-PC-001",
  "wifi_profile_hint": "makerspace-iot"
}
```

Required fields are `schema_version`, `device_class`, `asset_tag`,
`install_location`, and `oms_claim_url`. `expected_build_target` lets an
installer catch a wrong firmware image before mounting. `wifi_profile_hint`
should name the intended SSID/profile but must not contain passwords.

## People counter (`seeed_xiao_esp32s3`)

### QR label fields

- `device_class`: `people_counter`
- `expected_build_target`: `seeed_xiao_esp32s3`
- Include `mounting_height_m`, `view_direction`, and `counting_zone` when the
  OMS claim page can pre-fill them.

### Install checklist

1. Confirm the camera lens is clean and the XIAO ESP32-S3 Sense ribbon cable is
   seated.
2. Mount above the doorway or area boundary with the camera pointed at the
   counting zone, not at public/private work surfaces.
3. Apply USB-C power and verify the LED enters `booting`, then `provisioning`.
4. Scan the QR label in OMS and confirm the asset/location match the physical
   label.
5. Join the `ForgeKey-Setup-XXXX` captive portal if WiFi is not preloaded.
6. Wait for `connected` and verify OMS receives an occupancy message.
7. Send `identify` from OMS and confirm the correct unit blinks.
8. Send `capture` only if privacy policy allows it; verify the framing image in
   OMS, then disable/cover any commissioning-only view if required.

## Temperature sensor (`seeed_xiao_esp32s3_temperature`)

### QR label fields

- `device_class`: `temperature_sensor`
- `expected_build_target`: `seeed_xiao_esp32s3_temperature`
- Include `sensor_model` (`DHT21`/`AM2301`) and `sample_location`.

### Install checklist

1. Place the probe away from vents, direct sun, soldering fumes, and hot tools.
2. Verify the data lead is on the configured DHT pin and strain-relieved.
3. Power the device and complete WiFi/OMS claim from the QR code.
4. Confirm `connected` status and a plausible first reading in OMS.
5. Compare against a known thermometer; document any calibration offset in OMS.
6. Confirm the local status page is reachable if the unit is mains powered.

## Cabinet lock (`esp32c6-lock/`)

### QR label fields

- `device_class`: `cabinet_lock`
- `expected_build_target`: `esp32c6-lock`
- Include `cabinet_id`, `door_swing`, `key_override_location`, and emergency
  contact information.

### Install checklist

1. Bench-test the solenoid, reed switch, latch supervisor, IR beam, and mortise
   key switch before mounting.
2. Mount so manual override remains accessible and documented.
3. Confirm the lock web status page is reachable on the local network.
4. Scan the QR label in OMS and bind the cabinet asset.
5. Run `unlock` with an authorized credential, then verify reed/latch state and
   relock behavior.
6. Test `lockout`/`clear_lockout` policy in OMS if enabled for the deployment.
7. Record emergency-open procedure and physical key custody.

## ePaper display (`seeed_xiao_epaper`)

### QR label fields

- `device_class`: `epaper_display`
- `expected_build_target`: `seeed_xiao_epaper`
- Include `display_id`, `asset_id`, `wake_interval_min`, and battery install
  date.

### Install checklist

1. Fully charge the panel before provisioning.
2. Confirm the printed `display_id` matches the OMS ePaper display row.
3. Mount where WiFi RSSI is adequate during the short wake window.
4. Trigger a manual refresh from OMS and verify the expected image appears.
5. Confirm battery health telemetry posts after wake.
6. Do not enable the local web status page; this class should preserve battery
   and report through OMS instead.

## Label placement

- Put the QR label where an operator can scan it without removing the device.
- Do not place WiFi passwords, JWTs, private keys, or provisioning tokens in the
  QR payload.
- If the label includes a MAC address, use lowercase bare 12-hex form to match
  MQTT topics.
