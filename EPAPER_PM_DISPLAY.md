# Seeed XIAO 7.5" ePaper PM-display

A ForgeKey device class that displays a preventive-maintenance status
line (days until next service) for the asset it's physically mounted
to. Builds against the `seeed_xiao_epaper` PlatformIO env.

## Hardware

- **Product**: Seeed Studio XIAO 7.5" ePaper Panel, SKU 6416
  ([product page](https://www.seeedstudio.com/XIAO-7-5-ePaper-Panel-p-6416.html),
  [wiki](https://wiki.seeedstudio.com/xiao_075inch_epaper_panel/),
  [Arduino cookbook](https://wiki.seeedstudio.com/xiao_075inch_epaper_panel_arduino/))
- **Panel**: GoodDisplay GDEY075T7, 800×480 monochrome
- **Driver IC**: UC8179 (`BOARD_SCREEN_COMBO=502` in Seeed_GFX)
- **MCU**: Seeed XIAO ESP32-C3 (ships in the box; pin-compatible
  with other XIAO modules but the cookbook + this firmware target
  the C3)
- **Battery**: 2000 mAh LiPo + ETA9740E8A charger; status surfaces
  via the on-board LED1/LED2/LED3
- **Battery ADC**: **not exposed** to the XIAO socket on the SKU
  6416 driver board (verified against the official schematic PDF).
  The OMS battery endpoint receives a placeholder 100% until the
  hardware design routes a divider or a future revision adds one.

### Pin map (driver-board → XIAO socket)

Pulled from `Seeed_GFX/User_Setups/EPaper_Board_Pins_Setups.h`,
`USE_XIAO_EPAPER_DRIVER_BOARD` block — use the `Dx` board-label
macros, not raw GPIOs, so the same firmware works if you swap in a
different XIAO module later.

| Signal | XIAO label |
|--------|-----------|
| RST    | D0        |
| CS     | D1        |
| BUSY   | D2        |
| DC     | D3        |
| SCK    | D8        |
| MISO   | D9        |
| MOSI   | D10       |

## Build + flash

```bash
# From /home/ian/gascity/ForgeKey
pio run -e seeed_xiao_epaper                    # build
pio run -e seeed_xiao_epaper -t upload          # flash
pio device monitor -e seeed_xiao_epaper -b 115200  # serial logs
```

The Seeed_GFX library is pulled directly from upstream GitHub by
`platformio.ini` (`https://github.com/Seeed-Studio/Seeed_Arduino_LCD.git`).
First build pulls it; subsequent builds use the cached copy.

## What you'll see on first flash

1. **No display_id in NVS** (fresh board) → panel paints an
   "Awaiting provisioning" card showing the device MAC. The
   firmware does not enter the wake-cycle / sleep loop; staff at
   the bench can read the MAC, add an `EPaperDisplay` row in OMS
   admin, then set `epaper/did` in NVS (see "Provisioning" below).
2. **display_id present + WiFi up** → panel runs a wake-cycle:
   - GET `/api/forgekey/epaper/<id>/image.png` → drains body and
     paints a placeholder "Image fetched" card (PNG decode lands
     in hardware-pass-2; for pass-1 we verify the HTTP round-trip
     and panel paint independently).
   - 404 or 409 from OMS → "Display not bound" card.
   - 304 → keep current paint.
   - POST `/api/forgekey/epaper/<id>/battery/` with placeholder 100%.
   - Persist new ETag to NVS, request deep sleep for
     `DEFAULT_WAKE_INTERVAL_MIN` (default 60 min).

## Provisioning a display_id at the bench

The device_id lives in NVS at namespace `epaper`, key `did`. Two
ways to set it during testing:

**1. From a Python serial shell** (preferred, no reflash):
```python
import esptool, serial
# Send: `nvs_set epaper did string <uuid>` via the firmware's
# serial REPL (TODO — add this REPL command in pass-2).
```

**2. Reflash with a sentinel default** for one boot:
- Set `-DEPAPER_DEBUG_DISPLAY_ID="\"00000000-0000-0000-0000-000000000000\""`
  in `platformio.ini` and have `loadFromNvs()` fall back to it
  (also TODO for pass-2).

For tonight's bench session, easiest path is to flash once, paint
the unprovisioned card, copy the MAC, create an `EPaperDisplay`
row in OMS admin, and then manually `idf.py monitor` + use the
ESP-IDF `nvs_get` tool to confirm wiring.

## OMS contract (recap)

| Method | Path                                              | Response                                              |
|--------|---------------------------------------------------|-------------------------------------------------------|
| GET    | `/api/forgekey/epaper/<display_id>/image.png`     | 200 PNG + `ETag`, or 304, or 404, or 409 (unbound)    |
| POST   | `/api/forgekey/epaper/<display_id>/battery/`      | 200 on persist; 400 on payload error; 404 on unknown |

Both endpoints are `AllowAny` because the firmware has no
persistent JWT credential at this device class. The image content
is information already visible on the panel mounted to the asset,
so the exposure surface is narrow.

## Status

This is **hardware-pass-1**. The build + panel paint + HTTP
round-trip are wired, but the actual PNG → e-paper scanline path
is intentionally a placeholder so the bench session can verify
wiring + driver + HTTP independently before chasing decoder bugs.

### Open TODOs (hardware-pass-2)

- **PNG → e-paper draw** in `fetchImage()`. Pseudocode is inline
  in the cpp; uses PNGdec to stream scanlines into the Seeed_GFX
  frame buffer, then `g_panel.update()` flips the panel.
- **NVS-driven cadence** — read `FORGEKEY_EPAPER_WAKE_INTERVAL_MINUTES`
  from NVS so the OMS dashboard can tune per panel without a
  reflash.
- **Serial REPL `nvs_set`** so bench operators don't need
  `nvs_get`/external tools to bind a panel.
- **Battery sense path** if a future board rev adds an ADC line, or
  if operators hand-solder a divider onto `BAT_4V2`.

## Swap workflow (post-foundation)

When a panel reports low battery (or the on-board charger LED says
so today), ops should be able to:

1. Pick a charged twin panel from the spare-shelf pool.
2. In OMS, rebind the asset from the dying panel to the charged
   one. The next wake-cycle on the charged panel pulls the asset's
   latest PNG.
3. Take the dying panel off the asset, plug it into a charger;
   once fully charged it goes back on the spare-shelf and can be
   bound to another asset.

OMS-side swap helpers are a separate PR (`forgekey/epaper/swap/`
endpoint set + admin action).
