# ForgeKey People Counter

Area occupancy counting using XIAO ESP32-S3 Sense and camera.

## Features
- Camera-based people detection (motion detection, TFLite ready)
- Occupancy counting with stabilization
- MQTT publishing to OpenMakerSpace
- PSRAM support for frame buffering

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
`/<mac_address>/people_counter/occupancy`

Payload:
```json
{"count": 3, "timestamp": 12345}
```

## Current Implementation: Motion Detection

The current code uses motion detection for people counting:
- Detects pixel changes between frames
- Simple and works immediately
- May have false positives (lighting changes, etc.)

## Upgrade to TensorFlow Lite Person Detection

For proper ML-based person detection:

1. **Get Person Detection Model**
   ```bash
   # Download from Espressif
   wget https://github.com/espressif/esp-tflite-micro/raw/main/examples/person_detection/person_detect.tflite
   
   # Convert to C header
   xxd -i person_detect.tflite > src/detection/tflite_model.h
   ```

2. **Update platformio.ini**
   ```ini
   lib_deps = 
       espressif/esp32-camera
       knolleary/PubSubClient
       tanakamasayuki/TensorFlowLite_ESP32@^1.0.0
   ```

3. **Update person_detector.cpp** to use TFLite inference instead of motion detection

4. **Rebuild and flash**

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
- RAM usage: ~15%
- Flash usage: ~22%
