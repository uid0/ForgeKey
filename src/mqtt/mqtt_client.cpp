#include "mqtt_client.h"
#include "Arduino.h"

MqttClient mqttClient;

bool MqttClient::begin(const char* broker, int port, const char* jwt) {
    if (client) delete client;
    
    client = new PubSubClient(wifiClient);
    client->setServer(broker, port);
    jwtToken = jwt;
    
    return connect();
}

void MqttClient::setTopicPrefix(const char* mac) {
    topicPrefix = String("/") + mac + "/people_counter";
}

bool MqttClient::connect() {
    if (!client) return false;
    
    String clientId = "ForgeKey_" + String((uint32_t)ESP.getEfuseMac(), HEX);
    
    if (client->connect(clientId.c_str(), "forgemqtt", jwtToken.c_str())) {
        Serial.println("MQTT connected");
        return true;
    }
    
    Serial.printf("MQTT connect failed, rc=%d\n", client->state());
    return false;
}

bool MqttClient::publishOccupancy(int count) {
    if (!client || !client->connected()) {
        if (millis() - lastReconnectAttempt > 5000) {
            connect();
            lastReconnectAttempt = millis();
        }
        return false;
    }
    
    String topic = topicPrefix + "/occupancy";
    String payload = "{\"count\":" + String(count) + 
                    ",\"timestamp\":" + String(millis()) + "}";
    
    bool result = client->publish(topic.c_str(), payload.c_str());
    if (result) {
        Serial.println("Published: " + payload);
    }
    
    return result;
}

bool MqttClient::isConnected() {
    return client && client->connected();
}

void MqttClient::loop() {
    if (client) client->loop();
}

void MqttClient::end() {
    if (client) {
        client->disconnect();
        delete client;
        client = nullptr;
    }
}
