# Temperature-sensor Connector and Harness Notes

- **J1 DHT harness pinout:** pin 1 = 3V3, pin 2 = DATA/GPIO2/D1, pin 3 = GND.
- **Pull-up:** fit 4.7 kΩ to 10 kΩ from DATA to 3V3. Put it at the controller
  side when using long leads.
- **Cable:** twisted DATA/GND is preferred for runs longer than a short pigtail;
  avoid routing alongside solenoid or motor wiring.
- **Sensor placement:** mount in free air, away from MCU regulator heat, direct
  sun, vents, or condensation paths.
- **Acceptance check:** compare the first three published readings against a
  trusted room sensor and confirm OMS receives `/temperature_sensor/reading`.
