# Temperature-sensor BOM

| Qty | Item | Minimum requirements | Notes |
|---:|---|---|---|
| 1 | Seeed Studio XIAO ESP32-S3 | 3.3 V logic, WiFi | Firmware target `seeed_xiao_esp32s3_temperature`. |
| 1 | DHT21 / AM2301 sensor | 3.3 V compatible single-wire temperature/RH sensor | DHT22/AM2302 may be used for lab simulation only. |
| 1 | Pull-up resistor | 4.7 kΩ to 10 kΩ, 1/8 W or larger | DATA to 3V3 near the controller end. |
| 1 | 3-pin keyed connector | JST-XH or equivalent | Pin order: 3V3, DATA, GND. |
| 1 | USB-C cable or 5 V harness | Stable 5 V input | Permanent installs require strain relief. |
| 1 | Ventilated enclosure | Shields from splashes/direct airflow | Keep sensor away from regulator heat. |
