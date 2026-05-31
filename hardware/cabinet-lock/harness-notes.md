# Cabinet-lock Connector and Harness Notes

- **J1 solenoid:** two-conductor power cable sized for solenoid inrush. Route
  away from sensor lines. Put the flyback diode physically close to the coil or
  driver terminals.
- **J2 reed:** dry contact between GPIO1 and GND. Firmware interprets closed as
  LOW after debounce.
- **J3 IR beam:** provide 3V3, GPIO4 signal, and GND. If the receiver is 5 V
  logic, add level shifting before GPIO4.
- **J4 mortise:** dry contact between GPIO5 and GND. Use shielded/twisted cable
  in noisy cabinets.
- **J5 latch supervisor:** dry contact between GPIO6 and GND, aligned so LOW
  means latch locked.
- **J6 power:** 12 V and GND input. Tie controller ground, MOSFET source, sensor
  returns, and PSU negative together at a low-impedance point.
- **Acceptance check:** with the solenoid disconnected, verify each input changes
  state in telemetry. Then connect the solenoid, pulse once, and confirm the
  flyback diode orientation by checking for reset-free operation.
