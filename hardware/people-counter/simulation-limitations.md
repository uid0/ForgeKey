# People-counter Simulation Limitations

No Wokwi project is provided for this class because the simulator does not model
an XIAO ESP32-S3 Sense OV3660 camera, its parallel pixel bus, PSRAM-backed frame
capture, or TensorFlow Lite inference timing.

Use the committed SVG and pin manifest for wiring review. Validate actual builds
on a bench by checking camera initialization, JPEG capture, and OMS upload.
