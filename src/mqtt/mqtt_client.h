#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <PubSubClient.h>
#include <WiFiClient.h>
#include <functional>

class MqttClient {
public:
    using MessageHandler = std::function<void(const char* topic,
                                              const uint8_t* payload,
                                              unsigned int length)>;

    bool begin(const char* broker, int port, const char* jwtToken);
    void setTopicPrefix(const char* macAddress);

    // Override the per-device topics returned by OMS at registration.
    // If empty, defaults derived from the MAC are used.
    void setOccupancyTopic(const char* topic);
    void setReadingTopic(const char* topic);
    void setFirmwareTopic(const char* topic);
    void setFirmwareStatusTopic(const char* topic);
    void setConfigTopic(const char* topic);

    bool publishOccupancy(int count);
    // Publish a temperature/humidity reading on the device's reading topic.
    // Topic shape: forgekey/<mac>/temperature_sensor/reading. Payload:
    //   {"tempC": 21.4, "humidity": 47.1, "timestamp": <millis>}
    bool publishTemperature(float tempC, float humidity);
    // Publish OTA progress JSON on the firmware-status topic. The payload is:
    //   {"state": "<state>", "version": "...", "progress": 0..100, "error": "..."}
    // version/progress/error are optional; pass empty/-1 to omit. Best-effort:
    // returns false if not connected, but never blocks the OTA path.
    bool publishFirmwareStatus(const char* state,
                               const char* version,
                               int progress,
                               const char* error);
    bool subscribeFirmware(MessageHandler handler);
    bool subscribeConfig(MessageHandler handler);
    bool isConnected();
    void loop();
    void end();

    // Diagnostic accessors. lastSuccessfulPublishMs() returns 0 if nothing
    // has ever been published successfully (so disconnect logging can say
    // "never" instead of a misleading age).
    unsigned long lastSuccessfulPublishMs() const { return lastPublishMs; }
    int lastConnectRc() const { return lastConnectState; }

private:
    WiFiClient wifiClient;
    PubSubClient* client = nullptr;
    String topicPrefix;
    String occupancyTopic;  // resolved publish topic (people/door counter builds)
    String readingTopic;    // resolved publish topic (temperature sensor builds)
    String firmwareTopic;       // OTA dispatch topic (subscribe)
    String firmwareStatusTopic; // OTA progress topic (publish)
    String configTopic;         // credential-rotation / config command topic
    String jwtToken;
    String broker;
    int port = 1883;
    unsigned long lastReconnectAttempt = 0;
    unsigned long lastPublishMs = 0;       // millis() of last publish() == true
    int lastConnectState = 0;              // PubSubClient state after last connect attempt
    MessageHandler firmwareHandler;
    MessageHandler configHandler;

    bool connect();
    void resubscribeAll();
    static void staticCallback(char* topic, uint8_t* payload, unsigned int length);
};

extern MqttClient mqttClient;

#endif
