# ForgeKey Operator Runbook

This runbook is for installers and support operators managing ForgeKey devices
through OMS, MQTT, serial logs, and local status pages.

## Quick triage

1. Identify the physical device by asset tag or QR label.
2. Check OMS device state and latest telemetry timestamp.
3. Send `status` from OMS. If no ack arrives, check broker connectivity and
   retained `forgekey/<mac>/state`.
4. If the device is reachable on the LAN and the class supports it, open
   `http://<device-ip>/status.json`.
5. Use the LED/status table in `docs/STATUS_SEMANTICS.md` to decide whether the
   unit is provisioning, connected, degraded, in OTA, or in error.

## Safe support mode

Support mode temporarily increases diagnostics without permanently changing the
configuration. It enables ESP debug output, publishes periodic support
heartbeats, and exposes support fields in `status` and local web JSON.

Start for the default 15 minutes:

```json
{ "cmd": "support_mode", "action": "start" }
```

Start for a shorter window:

```json
{ "cmd": "support_mode", "action": "start", "duration_s": 300 }
```

Check or stop:

```json
{ "cmd": "support_mode", "action": "status" }
{ "cmd": "support_mode", "action": "stop" }
```

`run_diagnostics` is an alias that starts support mode and returns the same
ack fields. Durations are capped at 60 minutes. Do not leave support mode on
long-term because debug output and extra MQTT logs cost power and bandwidth.

## Local web status pages

- People-counter and temperature-sensor Arduino builds start a read-only page at
  `http://<device-ip>/` after WiFi/MQTT setup.
- JSON is available at `http://<device-ip>/status.json` and mirrors the `status`
  command payload, including heap, RSSI, WiFi health, power health, OTA slot,
  watchdog, capabilities, and support-mode fields.
- Cabinet-lock firmware already exposes its ESP-IDF lock status page/API.
- ePaper displays should not run a local web server because preserving wake time
  and battery is more important than interactive LAN diagnostics.

## Common workflows

### Commission a new device

1. Scan the QR label and confirm device class, expected firmware target, and
   location in OMS.
2. Power the device and wait for `provisioning`.
3. Complete captive-portal WiFi setup if needed.
4. Wait for `connected`, then send `identify` and verify the correct unit blinks.
5. Send `status` and save the ack in the install ticket.
6. Run the variant checklist in `docs/INSTALLATION.md`.

### Diagnose a degraded device

1. Send `support_mode start` for 5-15 minutes.
2. Send `status` and compare RSSI, WiFi disconnect reason, MQTT state, heap,
   power alarms, and watchdog fields against the last healthy ticket.
3. Check whether the LED is `degraded` or `error`.
4. If MQTT is unreliable but LAN works, open the local status JSON.
5. If the device repeatedly reboots or reports brownout, inspect power before
   replacing firmware.

### Locate a device

Send:

```json
{ "cmd": "identify", "duration_s": 60 }
```

The LED enters the `identify` override and then resumes its previous state.

### OTA update support

1. Confirm the artifact target matches the QR `expected_build_target` and OMS
   hardware class.
2. Dispatch the OTA from OMS.
3. Watch firmware status: `received` → `downloading` → `verifying` →
   `rebooting` → `applied`.
4. The LED should show `ota` during the update window.
5. If status reports `rejected` or `failed`, capture the error code before
   retrying.

### Retire or factory reset

Use signed OMS lifecycle commands only:

- `retire`: clears device identity but retains WiFi for controlled removal.
- `factory_reset`: wipes identity and local WiFi credentials.
- `reprovision`: clears identity and re-enters enrollment while retaining WiFi.

After issuing a lifecycle command, wait for the final ack and retained offline
state before removing power.

## Escalation data to attach

- Asset tag and QR payload or photo.
- Last `status` ack JSON.
- Support-mode ack and any `DEBUG/SUPPORT` logs.
- Local `/status.json` if MQTT is down.
- Serial boot log when physical access is available.
- Firmware version, build target, and OTA partition state.
