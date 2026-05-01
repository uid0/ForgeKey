#include "temperature_sensor.h"

#include <DHTesp.h>
#include <math.h>

TemperatureSensor temperatureSensor;

namespace {
DHTesp dht;
}  // namespace

bool TemperatureSensor::begin(int pin) {
    dataPin = pin;
    // DHT 21 (AM2301) is electrically identical to DHT22 / AM2302 — same
    // one-wire protocol, different package. The DHTesp driver only exposes
    // AM2302; using it is the correct choice for AM2301 sensors too.
    dht.setup(pin, DHTesp::AM2302);
    initialised = true;
    Serial.printf("[TEMP] DHT21 initialised on GPIO %d\n", pin);
    return true;
}

TemperatureReading TemperatureSensor::read() {
    TemperatureReading r{NAN, NAN, false};
    if (!initialised) {
        Serial.println("[TEMP] read: not initialised");
        return r;
    }

    TempAndHumidity v = dht.getTempAndHumidity();
    DHTesp::DHT_ERROR_t err = dht.getStatus();
    if (err != DHTesp::ERROR_NONE || isnan(v.temperature) || isnan(v.humidity)) {
        Serial.printf("[TEMP] read FAILED: status=%d t=%.2f rh=%.2f\n",
                      (int)err, v.temperature, v.humidity);
        return r;
    }

    r.tempC = v.temperature;
    r.humidity = v.humidity;
    r.ok = true;
    return r;
}
