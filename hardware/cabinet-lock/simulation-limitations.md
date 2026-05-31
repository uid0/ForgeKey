# Cabinet-lock Simulation Limitations

The Wokwi project verifies GPIO polarity and state-machine-friendly inputs with
buttons and an LED standing in for the solenoid driver. It does not simulate:

- solenoid inrush current, heating, force, or duty-cycle limits;
- MOSFET safe-operating area or gate-charge behavior;
- flyback diode recovery, voltage clamp, or EMI;
- reed switch bounce beyond simple button toggles;
- IR beam alignment, ambient light, or receiver output-level compatibility.

Bench validation with the production lock, supply, MOSFET, diode, and harness is
required before field installation.
