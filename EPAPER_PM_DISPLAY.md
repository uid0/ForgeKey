# XIAO 7.5" ePaper PM-display variant

A ForgeKey device class that displays a preventive-maintenance status
line (days until next service) for the asset it's physically mounted
to. Builds against the `seeed_xiao_esp32s3_epaper` PlatformIO env.

## Hardware

- Seeed XIAO ESP32-S3 (`board = seeed_xiao_esp32s3`)
- Seeed Wio E-Paper 7.5" panel (800×480, 1-bpp)
- 3.7V LiPo cell on the XIAO battery connector; voltage divider on
  ADC1_CH0 (GPIO 1) reports cell voltage
- SPI panel wired to the XIAO's default SPI pins; chip-select on a
  GPIO of your choice (set in the GxEPD2 init call once that lands)

## Lifecycle

Asymmetric to the other ForgeKey variants — the panel does **not**
stay online and tick. Each wake-cycle runs once, end-to-end:

1. **Wake** from deep sleep (timer-based,
   `FORGEKEY_EPAPER_WAKE_INTERVAL_MINUTES`, default 60 min).
2. **WiFi connect** via the shared `wifi_setup/captive` path.
3. **GET** `/api/forgekey/epaper/<display_id>/image.png` with the
   last-known ETag in `If-None-Match`. On 304 keep the current
   paint; on 200 decode the PNG and flash the panel.
4. **Sample** battery on ADC1_CH0; convert to 0..100% and **POST**
   `/api/forgekey/epaper/<display_id>/battery/` with
   `{"percent": N}`. OMS warns to Sentry below the configured floor
   (`FORGEKEY_EPAPER_LOW_BATTERY_PERCENT`, default 20%).
5. **Persist** the new ETag to NVS, request deep sleep.

`display_id` is provisioned into NVS at enrollment time; the OMS
admin shows it on the `EPaperDisplay` row.

## OMS contract

Server-side rendering, panel-side flashing only — keeps the firmware
simple and the layout in one place. See `backend/forgekey/views.py`
(`EPaperDisplayImageView`, `EPaperDisplayBatteryView`) and the
permission-matrix entries in `docs/API_PERMISSION_MATRIX.md`.

| Method | Path                                              | Response                                              |
|--------|---------------------------------------------------|-------------------------------------------------------|
| GET    | `/api/forgekey/epaper/<display_id>/image.png`     | 200 PNG + `ETag`, or 304, or 404, or 409 (unbound)    |
| POST   | `/api/forgekey/epaper/<display_id>/battery/`      | 200 on persist; 400 on payload error; 404 on unknown id |

Both endpoints are `AllowAny` because the firmware has no persistent
JWT credential at this device class. The image content is
information already visible on the panel mounted to the asset, so
the exposure surface is narrow.

## Status

This is the **foundation** scaffold:

- `[env:seeed_xiao_esp32s3_epaper]` env wired in `platformio.ini`.
- `src/capabilities/epaper_pm/` capability registered against the
  capability registry.
- HTTP GET / POST wiring complete with `If-None-Match` short-circuit
  + battery telemetry.
- `main.cpp` guards updated so the camera / people-counter paths are
  excluded on the ePaper build (same pattern as the temperature
  variant).

**Open TODOs** for hardware-pass-1 (require physical bring-up):

- Initialise the GxEPD2 driver in `EPaperPmCapability::setupFn()`
  (display rotation, partial-refresh tuning).
- Wire the PNG → e-paper scanline draw in `fetchAndRenderImage()`.
  Pseudocode is inline in that function — uses PNGdec to stream
  scanlines into the GxEPD2 frame buffer.
- Validate the LiPo voltage→percent curve against a real cell.
  Current implementation is a linear approximation (3.3V→0%,
  4.2V→100%); a piecewise table will be more accurate.
- Wire `FORGEKEY_EPAPER_WAKE_INTERVAL_MINUTES` to NVS so the OMS
  operator dashboard can tune cadence per panel without a reflash.

## Swap workflow (post-foundation)

When a panel reports low battery, ops should be able to:

1. Pick a charged twin panel from the spare-shelf pool.
2. In OMS, rebind the asset from the dying panel to the charged
   one. The next wake-cycle on the charged panel pulls the asset's
   latest PNG.
3. Take the dying panel off the asset, plug it into a charger; once
   fully charged it goes back on the spare-shelf and can be bound
   to another asset.

OMS-side swap helpers are a separate PR (`forgekey/epaper/swap/`
endpoint set + admin action).
