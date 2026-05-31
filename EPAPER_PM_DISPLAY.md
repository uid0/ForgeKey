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
  The stock firmware reports `battery.available=false` in health
  telemetry instead of a placeholder percentage. A hardware spin or
  field mod can route `BAT_4V2` through a divider to an ADC-capable
  XIAO pin and define `FORGEKEY_EPAPER_BATTERY_ADC_PIN`,
  `FORGEKEY_EPAPER_BATTERY_DIVIDER_NUM`,
  `FORGEKEY_EPAPER_BATTERY_DIVIDER_DEN`,
  `FORGEKEY_EPAPER_BATTERY_EMPTY_MV`, and
  `FORGEKEY_EPAPER_BATTERY_FULL_MV` to enable voltage/percent reporting.

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

### Repeatable hardware artifacts

The source-controlled hardware pack for this device class is
[`hardware/epaper-display/`](hardware/epaper-display/). It includes the
machine-readable [`pin-manifest.json`](hardware/epaper-display/pin-manifest.json),
rendered [`wiring.svg`](hardware/epaper-display/wiring.svg),
[`bom.md`](hardware/epaper-display/bom.md),
[`harness-notes.md`](hardware/epaper-display/harness-notes.md), and
[`simulation-limitations.md`](hardware/epaper-display/simulation-limitations.md).
The manifest documents ePaper SPI/control wiring plus the battery/power note
that SKU 6416 does not expose a battery ADC line to the XIAO socket.

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
firmware, mount the panel, do the rest from OMS." The ePaper SKU still
skips the MAC/MQTT enrollment used by the people-counter and
temperature-sensor firmware, but it now uses display_id-keyed HTTPS
polling for remote OTA, desired wake cadence, commands, health, and
content.

1. **No display_id in NVS** (fresh board) → firmware generates a
   v4 UUID from `esp_random()`, persists it to NVS at namespace
   `epaper`, key `did`. Same UUID survives subsequent boots.
2. **WiFi connects** via either the captive portal or a
   `secrets_local.h` predefined network.
3. **Wake-cycle control**: GET `/api/forgekey/epaper/<did>/firmware.json`
   and apply any signed OTA spec whose policy matches this hardware,
   then GET `/api/forgekey/epaper/<did>/desired.json` for `wake_min`
   and one-shot commands.
4. **Content fetch**: GET `/api/forgekey/epaper/<did>/image.png`. The
   server auto-creates an unbound `EPaperDisplay` row on first
   contact and responds 409.
5. On **409**, panel paints a bind QR encoding
   `<oms-base-url>/forgekey/epaper/bind?did=<uuid>`. A staff
   member scans with a phone, picks an asset from the mobile bind
   page, POSTs to the OMS `/bind/` endpoint. The page is
   staff-JWT gated.
6. Next wake-cycle: GET `image.png` returns 200 with a fresh PNG.
   Firmware decodes via PNGdec (per-scanline thresholding of the
   RGB565 conversion's green channel) and full-paints the panel.
7. Subsequent wakes that hit a matching ETag return 304; panel
   keeps its current paint and uses adaptive deep-sleep backoff so
   repeatedly quiet displays poll less often.

Other responses:
- **304** → keep current paint.
- **404** → panel was marked retired in OMS; paint a "Panel
  retired" card.
- Other (transport / decode failure) → keep current paint, retry
  next wake.

After a wake cycle, the firmware persists its ETag/backoff state,
POSTs `/api/forgekey/epaper/<did>/health/`, and deep-sleeps. The
active cadence starts from `DEFAULT_WAKE_INTERVAL_MIN` minutes (default
60) or the last OMS `wake_min` desired-state value. Repeated 304/no-change
wakes double from that configured value toward a 12-hour quiet cap, and
HTTP/transport failures retry on a shorter 15→30→60→120→240 minute ladder.

## OMS contract

| Method | Path                                          | Response                                                        |
|--------|-----------------------------------------------|-----------------------------------------------------------------|
| GET    | `/api/forgekey/epaper/<did>/firmware.json`    | 200 signed OTA dispatch JSON; 204/404 no update. Polled once per wake before content. |
| POST   | `/api/forgekey/epaper/<did>/firmware/status/` | OTA lifecycle status (`received`, `rejected`, `downloading`, `verifying`, `rebooting`, `failed`) plus OTA slot health. |
| GET    | `/api/forgekey/epaper/<did>/desired.json`     | 200 desired state; 204/404 no changes. Supports `wake_min`, `force_refresh`, `retired`, `identify`, `factory_reset`, and `command`/`commands` objects. |
| POST   | `/api/forgekey/epaper/<did>/command/status/`  | Best-effort command acknowledgement with `command`, optional `command_id`, and `state`. |
| GET    | `/api/forgekey/epaper/<did>/image.png`        | 200 PNG + `ETag`; 304 on matching `If-None-Match`; 409 unbound; 404 retired. |
| POST   | `/api/forgekey/epaper/<did>/bind/`            | Staff JWT. Body `{"asset_id": "..."}`. Called by the mobile bind page, NOT by firmware. |
| POST   | `/api/forgekey/epaper/<did>/health/`          | Per-wake health fields listed below; replaces placeholder battery-only telemetry. |

`image.png`, `firmware.json`, `desired.json`, command status, firmware
status, and health are display_id-keyed HTTPS endpoints so the panel can
stay off MQTT for battery life. The PNG content is information already
visible on the panel mounted to the asset, so the exposure surface is
narrow; OTA images remain protected by the signed firmware spec and
on-device signature verification.

### Desired-state and command schema

`desired.json` may return a direct desired object or wrap it under
`{"desired": {...}}`. Recognized values:

```json
{
  "desired": {
    "wake_min": 60,
    "force_refresh": true,
    "retired": false,
    "identify": false,
    "factory_reset": false
  },
  "command": {"id": "cmd-123", "name": "identify"}
}
```

Commands may use `name`, `command`, `cmd`, or `action`; supported names
are `force_refresh`, `retire`, `unretire`/`activate`, `identify`, and
`factory_reset`. Boolean desired values are accepted for state convergence;
command acknowledgements are emitted for command objects or desired objects
that include `command_id`. `force_refresh` clears the persisted ETag before fetching
`image.png`; `retire` persists a local retired flag and paints the retired
card; `identify` paints an identification card for one cycle;
`factory_reset` acknowledges, paints a reset card, clears ePaper NVS and
WiFiManager credentials, then reboots into setup.

### Health payload

Each wake POSTs `/health/` after rendering and before deep sleep. OMS should
persist at least these ePaper-specific fields:

- `last_image_etag`
- `unchanged_count`
- `failure_count`
- `wake_interval_min`
- `configured_wake_min`
- `render_status`
- `cycle_result`
- `last_http_status`
- `retired`
- `battery.available`, `battery.source`, `battery.reason` or
  `battery.voltage_mv`/`battery.percent` when ADC sensing is compiled in

The firmware also appends existing OTA slot health and board-manifest health
fields to the same payload.

## Remaining TODOs

- **OMS UI/API plumbing** for publishing `desired.json` values and consuming
  `/health/` and `/command/status/` for operator visibility.
- **Battery sense hardware** if a future board revision adds an ADC line, or
  if operators hand-solder a divider onto `BAT_4V2` and compile the ADC
  build flags documented above.

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
