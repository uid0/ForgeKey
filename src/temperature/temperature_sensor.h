#ifndef TEMPERATURE_SENSOR_H
#define TEMPERATURE_SENSOR_H

#include <Arduino.h>

// Thin wrapper around the DHTesp driver for DHT 21 / AM2301 sensors. Only
// compiled when FORGEKEY_TEMPERATURE_SENSOR is defined; the camera/detection
// path is mutually exclusive with this one.
//
// DHT 21 is a one-wire temperature + relative humidity sensor. It tolerates a
// minimum sample interval of ~2s; we sample less frequently than that to keep
// thermal self-heating low and to amortise the (slow) bit-banged read.

struct TemperatureReading {
    float tempC;     // degrees Celsius
    float humidity;  // % relative humidity
    bool  ok;        // false on read error or NaN; tempC/humidity undefined
};

class TemperatureSensor {
public:
    // pin: GPIO connected to the DHT data line. Pull-up is provided by the
    // DHTesp driver via INPUT_PULLUP, but a 4.7k–10k external resistor is
    // recommended on long leads.
    bool begin(int pin);

    // Triggers a fresh read. Returns ok=false on protocol/CRC error or if the
    // driver hands back NaN (sensor disconnected, brown-out, etc.).
    TemperatureReading read();

private:
    int dataPin = -1;
    bool initialised = false;
};

extern TemperatureSensor temperatureSensor;

#endif
