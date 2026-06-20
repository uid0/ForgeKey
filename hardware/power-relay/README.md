# XIAO ESP32-C3 2-Channel Wi-Fi AC Relay

This hardware profile targets the Seeed Studio XIAO 2-Channel Wi-Fi AC Relay
(SKU 114993526) when it is flashed with ForgeKey instead of the stock ESPHome
image. The board is intended to switch two independent AC loads and includes a
BL0942 metering IC; this ForgeKey integration controls the two relay channels
and reports aggregate power/energy usage back to OMS.

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
| BL0942 UART RX | 20 | `FORGEKEY_POWER_RELAY_METER_RX_PIN` |
| BL0942 UART TX | 21 | `FORGEKEY_POWER_RELAY_METER_TX_PIN` |
| BL0942 UART baud | 2400 | `FORGEKEY_POWER_RELAY_METER_BAUD` |
| BL0942 sample interval | 15 s | `FORGEKEY_POWER_RELAY_METER_SAMPLE_INTERVAL_MS` |
| OMS usage publish interval | 60 s | `FORGEKEY_POWER_RELAY_USAGE_PUBLISH_INTERVAL_MS` |

The BL0942 pins are build defaults for the XIAO ESP32-C3 hardware profile in
this repo. Override them in `platformio.ini` if a board revision routes the
metering UART differently.

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
command id, target channel, requested state, both channels' last commanded
states, and the latest metering snapshot when one is available.

## OMS usage telemetry

The relay publishes periodic usage telemetry to `forgekey/<mac>/status` using
the additive `forgekey.status.v1` payload shape:

```json
{
  "schema_version": "forgekey.status.v1",
  "power_relay": {
    "channels": [
      {"channel": 1, "on": true},
      {"channel": 2, "on": false}
    ],
    "ready": true,
    "metering": {
      "chip": "BL0942",
      "enabled": true,
      "valid": true,
      "voltage_v": 120.1,
      "current_a": 0.425,
      "power_w": 50.9,
      "energy_kwh": 0.01234,
      "frequency_hz": 60.0,
      "sample_age_ms": 1200,
      "samples": 42,
      "checksum_errors": 0
    }
  },
  "reason": "periodic"
}
```

OMS should treat the BL0942 values as aggregate line-side telemetry for the
relay module, not per-channel measurements. `status`/`ping` responses and
`power_set` acknowledgements include the same `power_relay.metering` object so
operators can request an immediate snapshot.

## Notes

- Channel state is restored from NVS as the last commanded state. Because the
  relays are latching, the physical contacts should retain their state across a
  reboot, but ForgeKey cannot sense contact position on this hardware profile.
- BL0942 calibration defaults follow ESPHome's published BL0942 references.
  Override `FORGEKEY_POWER_RELAY_BL0942_*_REF` build flags after bench
  calibration if OMS needs billing-grade accuracy.
