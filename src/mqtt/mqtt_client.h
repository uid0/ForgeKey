#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <PubSubClient.h>
#include <WiFiClient.h>

class MqttClient {
public:
    bool begin(const char* broker, int port, const char* jwtToken);
    void setTopicPrefix(const char* macAddress);
    bool publishOccupancy(int count);
    bool isConnected();
    void loop();
    void end();
    
private:
    WiFiClient wifiClient;
    PubSubClient* client = nullptr;
    String topicPrefix;
    String jwtToken;
    unsigned long lastReconnectAttempt = 0;
    
    bool connect();
};

extern MqttClient mqttClient;

#endif
