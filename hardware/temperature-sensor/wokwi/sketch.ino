// ForgeKey temperature-sensor Wokwi smoke test.
// Wokwi models DHT22; production hardware uses DHT21/AM2301 on the same
// single-wire protocol and firmware default GPIO2 (XIAO D1).

#include <DHTesp.h>

constexpr int kDhtPin = 2;
DHTesp dht;

void setup() {
  Serial.begin(115200);
  dht.setup(kDhtPin, DHTesp::AM2302);
  Serial.println("ForgeKey temperature-sensor DHT smoke test on GPIO2");
}

void loop() {
  TempAndHumidity reading = dht.getTempAndHumidity();
  Serial.print("tempC=");
  Serial.print(reading.temperature, 1);
  Serial.print(" humidity=");
  Serial.println(reading.humidity, 1);
  delay(30000);
}
