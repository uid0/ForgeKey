#include "mqtt_client.h"
#include "Arduino.h"

MqttClient mqttClient;

void MqttClient::staticCallback(char* topic, uint8_t* payload, unsigned int length) {
    // Dispatch to the matching per-topic handler. PubSubClient delivers every
    // subscribed topic through the same callback, so we have to demux here.
    if (mqttClient.configTopic.length() &&
        mqttClient.configHandler &&
        mqttClient.configTopic == topic) {
        mqttClient.configHandler(topic, payload, length);
        return;
    }
    if (mqttClient.firmwareHandler) {
        mqttClient.firmwareHandler(topic, payload, length);
    }
}

bool MqttClient::begin(const char* brokerHost, int portNum, const char* jwt) {
    if (client) delete client;

    client = new PubSubClient(wifiClient);
    broker = brokerHost;
    port = portNum;
    client->setServer(broker.c_str(), port);
    client->setCallback(MqttClient::staticCallback);
    client->setBufferSize(1024);  // larger payloads for OTA dispatch JSON
    jwtToken = jwt;

    return connect();
}

void MqttClient::setTopicPrefix(const char* mac) {
    topicPrefix = String("/") + mac + "/people_counter";
    if (occupancyTopic.length() == 0) {
        occupancyTopic = topicPrefix + "/occupancy";
    }
}

void MqttClient::setOccupancyTopic(const char* topic) {
    if (topic && *topic) occupancyTopic = topic;
}

void MqttClient::setFirmwareTopic(const char* topic) {
    if (topic && *topic) {
        firmwareTopic = topic;
        if (isConnected()) {
            client->subscribe(firmwareTopic.c_str());
        }
    }
}

void MqttClient::setConfigTopic(const char* topic) {
    if (topic && *topic) {
        configTopic = topic;
        if (isConnected()) {
            client->subscribe(configTopic.c_str());
        }
    }
}

bool MqttClient::connect() {
    if (!client) return false;

    String clientId = "ForgeKey_" + String((uint32_t)ESP.getEfuseMac(), HEX);

    if (client->connect(clientId.c_str(), "forgemqtt", jwtToken.c_str())) {
        Serial.println("MQTT connected");
        resubscribeAll();
        return true;
    }

    Serial.printf("MQTT connect failed, rc=%d\n", client->state());
    return false;
}

void MqttClient::resubscribeAll() {
    if (firmwareTopic.length() && firmwareHandler) {
        client->subscribe(firmwareTopic.c_str());
    }
    if (configTopic.length() && configHandler) {
        client->subscribe(configTopic.c_str());
    }
}

bool MqttClient::publishOccupancy(int count) {
    if (!client || !client->connected()) {
        if (millis() - lastReconnectAttempt > 5000) {
            connect();
            lastReconnectAttempt = millis();
        }
        return false;
    }

    if (occupancyTopic.length() == 0) return false;

    String payload = "{\"count\":" + String(count) +
                    ",\"timestamp\":" + String(millis()) + "}";

    bool result = client->publish(occupancyTopic.c_str(), payload.c_str());
    if (result) {
        Serial.println("Published: " + payload);
    }

    return result;
}

bool MqttClient::subscribeFirmware(MessageHandler handler) {
    firmwareHandler = handler;
    if (firmwareTopic.length() == 0 || !client) return false;
    if (!client->connected()) return false;
    return client->subscribe(firmwareTopic.c_str());
}

bool MqttClient::subscribeConfig(MessageHandler handler) {
    configHandler = handler;
    if (configTopic.length() == 0 || !client) return false;
    if (!client->connected()) return false;
    return client->subscribe(configTopic.c_str());
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
