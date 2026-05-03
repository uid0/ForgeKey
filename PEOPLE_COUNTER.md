# ForgeKey People Counter

Area occupancy counting using XIAO ESP32-S3 Sense and camera.

## Features
- Camera-based people detection (TensorFlow Lite Micro, with motion-detection fallback)
- Occupancy counting with stabilization
- MQTT publishing to OpenMakerSpace
- PSRAM support for frame buffering and the TFLite tensor arena

## Hardware
- Seeed XIAO ESP32-S3 Sense (OV3660 camera, 8MB PSRAM)
- USB-C cable for power/programming

## Configuration

Edit `src/main.cpp` with your settings:
```cpp
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* MQTT_BROKER = "mqtt.yourdomain.com";
const int MQTT_PORT = 1883;
const char* MQTT_JWT = "YOUR_JWT_TOKEN";
```

## Building & Flashing

```bash
# Build
~/.platformio/penv/bin/platformio run

# Flash
~/.platformio/penv/bin/platformio run --target upload

# Monitor serial output
~/.platformio/penv/bin/platformio device monitor
```

## MQTT Topic
`forgekey/<mac_address>/people_counter/occupancy`

The `<mac_address>` is the bare lowercase 12-hex MAC (no separators), e.g.
`aabbcc112233`. The OMS-side subscriber listens on the same topic shape
(`backend/forgekey/utils.py`).

After a successful registration, the OMS server returns the canonical
topic in `mqtt_topic_for_pings`, which is persisted to NVS and used as an
override on subsequent boots. If that stored value does not match the
`forgekey/<mac>/(people_counter|door_counter)/occupancy` shape, the
firmware ignores it and forces a re-registration on the next boot.

Payload:
```json
{"count": 3, "timestamp": 12345}
```

## Debugging publishes

Verbose serial logging is on by default — see [DEBUG.md](DEBUG.md) for the
expected boot trace and a symptom → cause table.

## Operator commands (manual smoke)

Per-device control plane:

- Subscribe (device): `forgekey/<mac>/command`
- Publish (device):   `forgekey/<mac>/status`

### Blink ("identify me")

Used to physically locate a device. Toggles a 1.33 Hz on/off LED override on
the onboard status LED. Period is `BLINK_PERIOD_MS` (default 750 ms,
overridable at build time via `-DBLINK_PERIOD_MS=NNN`).

Accepted command payloads:

```json
{"cmd":"blink"}                       // toggle
{"cmd":"blink","action":"start"}      // explicit on
{"cmd":"blink","action":"stop"}       // explicit off
{"cmd":"blink","action":"toggle"}     // explicit toggle
{"cmd":"blink","on":true}             // explicit on
{"cmd":"blink","on":false}            // explicit off
```

On every transition the device publishes one of:

```json
{"blink":"on"}
{"blink":"off"}
```

Manual smoke (replace `<mac>` with the bare 12-hex MAC printed at boot):

```bash
# Terminal 1: watch status
mosquitto_sub -h dms.oms-iot.com -t "forgekey/<mac>/status" -v

# Terminal 2: toggle on, then off
mosquitto_pub -h dms.oms-iot.com -t "forgekey/<mac>/command" -m '{"cmd":"blink"}'
mosquitto_pub -h dms.oms-iot.com -t "forgekey/<mac>/command" -m '{"cmd":"blink"}'
```

Expect: LED visibly alternating ~0.75 s on / 0.75 s off after the first send,
returns to the steady ~3 s heartbeat after the second. Each send produces
exactly one status message; repeated `{"on":true}` while already on is a no-op
and emits nothing.

## Detection Pipeline

### TensorFlow Lite Person Detection (active)

`PersonDetector` runs Espressif's pre-trained 96×96 grayscale `person_detect`
model (int8-quantized MobileNet) via TensorFlow Lite Micro on the ESP32-S3.

- **Model**: `src/detection/tflite_model.cpp` — `g_person_detect_model_data`
  (~300 KB in flash). Sourced from
  [esp-tflite-micro](https://github.com/espressif/esp-tflite-micro/tree/master/examples/person_detection).
- **Library**: `tanakamasayuki/TensorFlowLite_ESP32@^1.0.0` (Arduino-compatible
  TFLite Micro port; supports ESP32-S3).
- **Tensor arena**: ~120 KB (`81 KB + 39 KB scratch`), allocated in PSRAM via
  `heap_caps_malloc(MALLOC_CAP_SPIRAM)`. PSRAM is required.
- **Input pipeline**: camera grayscale frame → nearest-neighbor resize to
  96×96 → shift uint8 → int8 (`pixel - 128`) → `interpreter->Invoke()`.
- **Output**: dequantized `[no_person_score, person_score]`. A person is
  reported when `person_score > 0.6` (tunable via
  `PersonDetector::setPersonThreshold`).
- **Per-frame cost**: target ≤500 ms on ESP32-S3 @ 240 MHz. Inference time is
  printed to serial alongside the scores.
- **Motion pre-filter**: a quick pixel-diff against the previous 96×96 frame
  is computed but currently informational only (`DetectionResult.motionDetected`).
  Future optimization can skip `Invoke()` when no motion is seen.

### Motion-Detection Fallback (legacy)

If TFLite initialization fails (e.g. PSRAM unavailable, arena allocation
fails, model schema mismatch), `PersonDetector` falls back to the original
motion detector: pixel-diff against the previous frame, with a 5%
changed-pixel threshold. Marked legacy — kept only as a safety net so the
firmware still produces useful occupancy signals on a misconfigured board.

### Updating the Model

To regenerate `src/detection/tflite_model.cpp` from a newer `person_detect.tflite`:

```bash
wget https://github.com/espressif/esp-tflite-micro/raw/master/examples/person_detection/main/person_detect_model_data.cc \
     -O src/detection/tflite_model.cpp
sed -i 's|#include "person_detect_model_data.h"|#include "tflite_model.h"|' src/detection/tflite_model.cpp
```

The header `src/detection/tflite_model.h` only declares the externs; it does
not need to be regenerated unless the symbol names change.

## File Structure
```
src/
├── main.cpp              # Main loop
├── camera/               # Camera management
├── detection/            # Person detection (motion/TFLite)
├── counting/             # Occupancy counter
└── mqtt/                 # MQTT client
```

## Performance
- Detection interval: 2 seconds (configurable)
- MQTT publish: On change or every 10 seconds
- TFLite inference: ~hundreds of ms per frame on ESP32-S3 @ 240 MHz
- TFLite tensor arena: ~120 KB in PSRAM (model itself: ~300 KB in flash)
- DRAM usage: dominated by camera + WiFi/MQTT stacks (TFLite arena lives in PSRAM)
