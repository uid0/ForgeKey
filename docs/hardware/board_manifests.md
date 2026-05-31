# ForgeKey board manifests

These manifests are the hardware contract used by firmware capability activation. Firmware mirrors the same ownership data in `src/boards/board_manifest.*` for Arduino builds and `esp32c6-lock/main/boards/lock_board_manifest.*` for the ESP-IDF lock build.

## Seeed XIAO ESP32-S3 (`seeed_xiao_esp32s3`)

- **GPIO ownership**
  - `people_counter` / OV3660 camera: XCLK GPIO10, PCLK GPIO13, VSYNC GPIO38, HREF GPIO47, data GPIO15/17/18/16/14/12/11/48, SCCB SDA GPIO40, SCCB SCL GPIO39.
  - `status_led`: onboard orange LED on GPIO21, active-low output, no external pull required.
  - `temperature_sensor` variant: DHT data on GPIO2 (`D1`), expects an external pull-up.
  - `button`: not populated by default; may only activate from an explicit manifest/compile-time pin.
  - `mmwave_presence`: not populated by default; may only activate from an explicit manifest UART assignment.
  - BLE capabilities use the ESP32-S3 radio and do not claim GPIOs.
- **Boot strapping constraints**: avoid adding active drive circuitry on ESP32-S3 strapping pins GPIO0, GPIO3, GPIO45, and GPIO46.
- **Pull expectations**: GPIO21 LED is board-managed; DHT GPIO2 needs pull-up; camera lines are owned by the camera module.
- **ADC availability**: ADC1-capable GPIOs include 1-10; do not reuse camera GPIO10 in people-counter builds.
- **Buses**
  - Camera SCCB/I2C-like bus: GPIO40 SDA / GPIO39 SCL, camera-owned.
  - External I2C header, if used by future capabilities: keep on non-camera XIAO pins and declare ownership before activation.
  - SPI/UART: no shared peripheral is assigned by the default people-counter build.
- **Unsafe pins**: camera-owned pins above, strapping pins GPIO0/3/45/46, USB pins GPIO19/20 when USB CDC is required, and flash/PSRAM-reserved pins.

## Seeed XIAO ESP32-C3 ePaper (`seeed_xiao_epaper`)

- **GPIO ownership**
  - `epaper_pm` / Seeed 7.5-inch ePaper driver board: RST `D0`/GPIO2, CS `D1`/GPIO3, BUSY `D2`/GPIO4, DC `D3`/GPIO5, SCK `D8`/GPIO8, MISO `D9`/GPIO9, MOSI `D10`/GPIO10.
  - `status_led`: not activated on this target; no status LED GPIO is claimed.
  - `button` and `mmwave_presence`: not populated by default.
- **Boot strapping constraints**: GPIO2, GPIO8, and GPIO9 are ESP32-C3 strapping pins; the ePaper driver board is the only approved owner and must not be paralleled with other peripherals.
- **Pull expectations**: ePaper BUSY is driven by the panel; CS/DC/RST/SPI are driven by firmware; no battery ADC divider is routed by this carrier.
- **ADC availability**: ADC-capable GPIO0-4 exist on ESP32-C3, but GPIO2/3/4 are ePaper-owned in this build.
- **Buses**: SPI bus is dedicated to the panel (`D8/D9/D10` plus CS `D1`). No I2C/UART bus is assigned by default.
- **Unsafe pins**: ePaper-owned pins above, flash pins, USB/JTAG/programming pins when needed, and strap pins unless the manifest owner is the ePaper carrier.

## ESP32-C6 cabinet lock (`esp32c6-lock`)

- **GPIO ownership**
  - `cabinet_lock` solenoid output: GPIO0, active-high.
  - Reed switch: GPIO1 input with pull-up, active-low closed.
  - IR beam: GPIO4 input with pull-up, active-low broken.
  - Mortise input: GPIO5 input with pull-up, active-low active.
  - Latch supervisor: GPIO6 input with pull-up, active-low locked.
- **Boot strapping constraints**: avoid assigning new capability outputs to ESP32-C6 strapping pins GPIO8, GPIO9, and GPIO15.
- **Pull expectations**: lock sensor inputs expect normally-open/collector style sensors pulled up internally; solenoid GPIO0 must drive a transistor/MOSFET input, never the coil directly.
- **ADC availability**: ADC1 GPIO0-7 are physically available on ESP32-C6, but GPIO0/1/4/5/6 are lock-owned by this manifest.
- **Buses**: no I2C, SPI, or UART expansion bus is assigned by default; future buses must be added to the manifest before their capability can activate.
- **Unsafe pins**: lock-owned pins above, strapping pins GPIO8/9/15, flash pins, and USB/JTAG pins when used for service/debug.
