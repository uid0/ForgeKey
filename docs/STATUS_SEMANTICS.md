# ForgeKey Status and LED Semantics

ForgeKey devices use one shared status language across people-counter,
temperature-sensor, cabinet-lock, and ePaper classes. A device with no visible
LED must still use the same state names in MQTT, local web status, operator
runbooks, and OMS UI labels.

## Common states

| State | Operator meaning | LED pattern | When firmware should enter it |
|---|---|---|---|
| `booting` | Power is present and firmware has started. | Five fast flashes, then the next requested state. | Reset, power-on, or wake before hardware detection completes. |
| `provisioning` | The device is not fully online yet. It may be in captive portal, joining WiFi, enrolling with OMS, or subscribing to MQTT. | Repeating long-long-short-long pattern. | WiFi connection, captive-portal setup, first OMS enrollment, command subscription setup. |
| `connected` | Normal service. OMS should be receiving telemetry. | One short heartbeat about every three seconds. | MQTT connected and the device is not in a special workflow. |
| `degraded` | The device is running but at least one network, broker, or capability path needs attention. | One short flash about once per second. | MQTT disconnect after having been connected, recoverable sensor/capability faults, weak but usable connectivity. |
| `ota` | Firmware update is being downloaded, verified, or rebooted into. | Rapid continuous flash. | OTA dispatch accepted through the final reboot/applied status window. |
| `error` | Blocking fault. Operator action is likely required. | Continuous fast flash. | Captive portal failure, unrecoverable command/OTA errors, hardware fault that prevents the device class from operating. |
| `identify` | An operator is physically locating the unit. | Symmetric on/off blink, default 750 ms. | `identify` or `blink` command. This overrides the underlying state temporarily. |
| `retired` | Signed retirement was accepted; identity is being cleared. | Solid on until reboot/power removal. | `retire` lifecycle command after final state publish. |
| `factory_reset` | Signed factory reset was accepted; local credentials are being wiped. | Ten fast flashes before reboot. | `factory_reset` lifecycle command. |

## Priority rules

1. `identify` is an operator override and must not erase the underlying state.
   When the timer expires, the device resumes the state that was requested while
   identify was active.
2. `ota`, `factory_reset`, and `retired` outrank normal telemetry states.
3. `error` outranks `degraded`; use `degraded` only for recoverable service loss.
4. Battery/deep-sleep devices may report the state in telemetry instead of
   holding an LED pattern awake. ePaper displays should not extend wake time only
   to blink an LED.

## Local status surfaces

Powered classes should expose the same fields in all support surfaces:

- MQTT command acks on `forgekey/<mac>/status`.
- Local web status page, where enabled, at `http://<device-ip>/` and JSON at
  `http://<device-ip>/status.json`.
- Serial logs at 115200 baud.

Battery-first or deep-sleep classes may disable the local web page with
`FORGEKEY_DISABLE_LOCAL_STATUS_WEB` and should rely on OMS health telemetry.
