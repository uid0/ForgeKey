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
`platformio.ini`. First build pulls it; subsequent builds use the
cached copy. `ricmoo/QRCode` and `bitbank2/PNGdec` are pulled from
the PlatformIO registry.

## First-boot flow (flash once, walk away)

No manual NVS provisioning. The intended bring-up is "flash the
firmware, mount the panel, do the rest from OMS." This does **not**
yet include remote firmware reflashing for the ePaper SKU: the current
ePaper build skips the MAC/MQTT provisioning and OTA stack used by the
people-counter and temperature-sensor firmware.

1. **No display_id in NVS** (fresh board) → firmware generates a
   v4 UUID from `esp_random()`, persists it to NVS at namespace
   `epaper`, key `did`. Same UUID survives subsequent boots.
2. **WiFi connects** via either the captive portal or a
   `secrets_local.h` predefined network.
3. **Wake-cycle**: GET `/api/forgekey/epaper/<did>/image.png`. The
   server auto-creates an unbound `EPaperDisplay` row on first
   contact and responds 409.
4. On **409**, panel paints a bind QR encoding
   `<oms-base-url>/forgekey/epaper/bind?did=<uuid>`. A staff
   member scans with a phone, picks an asset from the mobile bind
   page, POSTs to the OMS `/bind/` endpoint. The page is
   staff-JWT gated.
5. Next wake-cycle: GET `image.png` returns 200 with a fresh PNG.
   Firmware decodes via PNGdec (per-scanline thresholding of the
   RGB565 conversion's green channel) and full-paints the panel.
6. Subsequent wakes that hit a matching ETag return 304; panel
   keeps its current paint and uses adaptive deep-sleep backoff so
   repeatedly quiet displays poll less often.

Other responses:
- **304** → keep current paint.
- **404** → panel was marked retired in OMS; paint a "Panel
  retired" card.
- Other (transport / decode failure) → keep current paint, retry
  next wake.

After a wake cycle, the firmware persists its ETag/backoff state and
deep-sleeps. The active cadence starts from `DEFAULT_WAKE_INTERVAL_MIN`
minutes (default 60), repeated 304/no-change wakes double toward a
12-hour quiet cap, and HTTP/transport failures retry on a shorter
15→30→60→120→240 minute ladder. The firmware only occasionally POSTs
`/api/forgekey/epaper/<did>/battery/` with the placeholder 100%
(SKU 6416 has no battery sense exposed to the XIAO socket — see
above), avoiding a second HTTP request on most wakes.

## OMS contract

| Method | Path                                          | Response                                                        |
|--------|-----------------------------------------------|-----------------------------------------------------------------|
| GET    | `/api/forgekey/epaper/<did>/image.png`        | 200 PNG + `ETag`; 304 on matching `If-None-Match`; 409 unbound; 404 retired |
| POST   | `/api/forgekey/epaper/<did>/bind/`            | Staff JWT. Body `{"asset_id": "..."}`. Called by the mobile bind page, NOT by firmware. |
| POST   | `/api/forgekey/epaper/<did>/battery/`         | Body `{"percent": 0..100}`. 200 on persist; 400/404 on error.   |

`image.png` and `battery/` are `AllowAny` — the firmware carries no
JWT. The PNG content is information already visible on the panel
mounted to the asset, so the exposure surface is narrow.

## Open TODOs

- **OMS-managed cadence UI** — firmware reads a `wake_min` NVS override,
  but OMS still needs a dashboard/control path to tune it per panel.
- **Remote ePaper OTA** — adaptive wake backoff only changes how often
  the panel checks display content. It does not re-enable the skipped
  MQTT OTA path. Remote reflashing needs a display_id-keyed HTTPS OTA
  polling endpoint (or a deliberately short MQTT awake window) plus OMS
  support to publish signed firmware specs for this device class.
- **Battery sense path** if a future board revision adds an ADC
  line, or if operators hand-solder a divider onto `BAT_4V2`.
- **MQTT command pathway** (force-refresh, etc.) — the ePaper
  device class deliberately skips the MAC-based MQTT enrollment
  used by other ForgeKey devices, so any command pathway needs a
  display_id-keyed alternative.

## Swap workflow

When a panel runs low or fails:

1. Pick a charged twin panel from the spare-shelf pool.
2. Mount it on the asset.
3. Scan the new panel's bind QR with a phone and pick the same
   asset on the bind page. The dying panel's binding is left
   alone; the new panel takes over.
4. (Optional) Mark the old panel inactive in OMS admin so it
   stops 409-ing if it boots from the shelf later. Once it does,
   it will paint the "Panel retired" card.

A dedicated `forgekey/epaper/swap/` admin action that rebinds the
asset in one click — rather than scanning the new panel — is a
follow-up.
