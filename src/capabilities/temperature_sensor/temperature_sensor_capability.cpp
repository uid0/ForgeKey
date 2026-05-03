// Temperature-sensor capability: thin wrapper around the existing DHTesp
// driver in src/temperature/. Compiled into the temperature build env only
// (the camera env's build_src_filter excludes both src/temperature/ and
// this directory).
//
// detect() attempts a real read; the DHT driver returns NaN when the sensor
// isn't physically attached or the bus is floating, so a single successful
// read is a reliable presence probe.

#if defined(FORGEKEY_TEMPERATURE_SENSOR) && !defined(FORGEKEY_DISABLE_TEMPERATURE_SENSOR)

#include "../capability.h"

#include "../../temperature/temperature_sensor.h"
#include "../../mqtt/mqtt_client.h"
#include "../../ota/ota_updater.h"
#include "../../provisioning/device_config.h"

namespace TemperatureSensorCapability {

bool detectFn();
void setupFn();
void tickFn();

namespace {
unsigned long g_lastSample = 0;
}

bool detectFn() {
    // Initialise the bus, then attempt one read. If the sensor isn't there,
    // DHTesp returns NaN and we skip the capability.
    if (!temperatureSensor.begin(FORGEKEY_DHT_PIN)) {
        return false;
    }
    TemperatureReading r = temperatureSensor.read();
    return r.ok;
}

void setupFn() {
    // begin() already ran in detectFn(); nothing else to do.
}

void tickFn() {
    unsigned long now = millis();
    if (now - g_lastSample < TEMPERATURE_SAMPLE_INTERVAL_MS) return;
    g_lastSample = now;

    TemperatureReading r = temperatureSensor.read();
    if (!r.ok) return;

    Serial.printf("[CAP/temperature_sensor] reading: %.2f C, %.2f%% RH\n",
                  r.tempC, r.humidity);

    if (!mqttClient.isConnected()) {
        Serial.println("[CAP/temperature_sensor] skip publish — MQTT not connected");
        return;
    }

    if (mqttClient.publishTemperature(r.tempC, r.humidity)) {
        Serial.println("[CAP/temperature_sensor] published reading");
        otaUpdater.markStableIfPending();
    } else {
        Serial.println("[CAP/temperature_sensor] failed to publish reading");
    }
}

}  // namespace TemperatureSensorCapability

REGISTER_CAPABILITY(temperature_sensor, "temperature_sensor",
                    TemperatureSensorCapability::detectFn,
                    TemperatureSensorCapability::setupFn,
                    TemperatureSensorCapability::tickFn,
                    "reading")

#endif
