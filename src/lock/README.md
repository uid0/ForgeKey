# Legacy Arduino Lock Prototype

The files in this directory are the older Arduino-based cabinet-lock
implementation that predated the ESP32-C6 / ESP-IDF lock target.

The active lock firmware now lives in `esp32c6-lock/` and builds with
native `idf.py`.

This directory is kept as reference material, but it is excluded from the
default Arduino firmware builds in `platformio.ini`.
