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
    void setFirmwareTopic(const char* topic);
    void setConfigTopic(const char* topic);

    bool publishOccupancy(int count);
    bool subscribeFirmware(MessageHandler handler);
    bool subscribeConfig(MessageHandler handler);
    bool isConnected();
    void loop();
    void end();

private:
    WiFiClient wifiClient;
    PubSubClient* client = nullptr;
    String topicPrefix;
    String occupancyTopic;  // resolved publish topic
    String firmwareTopic;   // OTA dispatch topic
    String configTopic;     // credential-rotation / config command topic
    String jwtToken;
    String broker;
    int port = 1883;
    unsigned long lastReconnectAttempt = 0;
    MessageHandler firmwareHandler;
    MessageHandler configHandler;

    bool connect();
    void resubscribeAll();
    static void staticCallback(char* topic, uint8_t* payload, unsigned int length);
};

extern MqttClient mqttClient;

#endif
