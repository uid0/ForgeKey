# XIAO ESP32-C3 2-Channel Wi-Fi AC Relay

This hardware profile targets the Seeed Studio XIAO 2-Channel Wi-Fi AC Relay
(SKU 114993526) when it is flashed with ForgeKey instead of the stock ESPHome
image. The board is intended to switch two independent AC loads and includes a
BL0942 metering IC; this ForgeKey integration currently controls the two relay
channels and reports their last commanded state.

> Safety: this is a mains-voltage device. Disconnect AC power before opening or
> wiring the enclosure, do not use USB while the relay is connected to AC mains,
> and keep each channel within the board rating.

## Build target

```bash
pio run -e seeed_xiao_esp32c3_2ch_relay
```

Production profile:

```bash
pio run -e seeed_xiao_esp32c3_2ch_relay_prod
```

## Pin map

The relay hardware uses latching relay coils. ForgeKey pulses a set or reset
coil for 100 ms rather than holding a GPIO high continuously.

| Function | GPIO | Default macro |
| --- | ---: | --- |
| Relay 1 set/on pulse | 5 | `FORGEKEY_POWER_RELAY_CH1_SET_PIN` |
| Relay 1 reset/off pulse | 4 | `FORGEKEY_POWER_RELAY_CH1_RESET_PIN` |
| Relay 2 set/on pulse | 7 | `FORGEKEY_POWER_RELAY_CH2_SET_PIN` |
| Relay 2 reset/off pulse | 6 | `FORGEKEY_POWER_RELAY_CH2_RESET_PIN` |
| Pulse length | 100 ms | `FORGEKEY_POWER_RELAY_PULSE_MS` |

## MQTT command

Relay control uses the existing signed command envelope on
`forgekey/<mac>/command`. Send `power_set` (or the compatibility alias
`relay_set`) with a 1-based `channel` and desired `on` value:

```json
{
  "schema_version": "forgekey.command.v1",
  "cmd": "power_set",
  "command_id": "cmd-power-001",
  "issued_at": "2026-06-18T12:00:00Z",
  "expires_at": "2026-06-18T12:05:00Z",
  "nonce": "n-power-001",
  "actor": "operator@example.com",
  "channel": 1,
  "on": true,
  "signature": "..."
}
```

The acknowledgement is published to `forgekey/<mac>/status` and includes the
command id, target channel, requested state, and both channels' last commanded
states.

## Notes

- Channel state is restored from NVS as the last commanded state. Because the
  relays are latching, the physical contacts should retain their state across a
  reboot, but ForgeKey cannot sense contact position on this hardware profile.
- BL0942 power metering is present on the board but is not yet wired into the
  ForgeKey telemetry path.
