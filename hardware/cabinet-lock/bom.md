# Cabinet-lock BOM

| Qty | Item | Minimum requirements | Notes |
|---:|---|---|---|
| 1 | ESP32-C6 controller board | 3.3 V GPIO, WiFi | Firmware project `esp32c6-lock`. |
| 1 | Electric solenoid/latch | 12 V nominal, current known | Size PSU and MOSFET for inrush and duty cycle. |
| 1 | Logic-level N-MOSFET | Vds >= 30 V, Id >= solenoid inrush, low Rds(on) at 3.3 V gate | Low-side switch. |
| 1 | Flyback diode | If >= coil current, Vr >= supply; Schottky or fast diode acceptable | Cathode to +12 V, anode to MOSFET drain/coil low side. |
| 1 | Gate resistor | 100 Ω to 220 Ω | GPIO0 to MOSFET gate. |
| 1 | Gate pulldown | 100 kΩ | MOSFET gate to GND so lock stays off during reset. |
| 1 | Reed switch | Normally closed preferred for closed-door = low | Door-position supervisor on GPIO1. |
| 1 | IR beam receiver/emitter | 3.3 V logic receiver or level shifted output | Item-present beam on GPIO4. |
| 1 | Mortise switch | Dry contact to GND | Manual/mechanical access indication on GPIO5. |
| 1 | Latch supervisor switch | Dry contact to GND | Locked-position feedback on GPIO6. |
| 1 | 12 V power supply | Rated above solenoid inrush and average duty | Share ground with MCU. |
| 5 | Keyed connectors | Locking terminal/JST as appropriate | Separate power/solenoid and signal connectors. |
