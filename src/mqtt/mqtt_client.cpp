#include "mqtt_client.h"
#include "Arduino.h"

MqttClient mqttClient;

namespace {
// Human-readable PubSubClient state codes. Numeric values defined in
// PubSubClient.h (MQTT_CONNECTION_TIMEOUT = -4, ..., MQTT_CONNECTED = 0,
// MQTT_CONNECT_BAD_PROTOCOL = 1, ...).
const char* mqttStateName(int state) {
    switch (state) {
        case -4: return "CONNECTION_TIMEOUT (no broker response)";
        case -3: return "CONNECTION_LOST (TCP dropped)";
        case -2: return "CONNECT_FAILED (TCP connect to broker failed)";
        case -1: return "DISCONNECTED (clean disconnect)";
        case  0: return "CONNECTED";
        case  1: return "CONNECT_BAD_PROTOCOL (server doesn't support our MQTT version)";
        case  2: return "CONNECT_BAD_CLIENT_ID (server rejected client id)";
        case  3: return "CONNECT_UNAVAILABLE (broker unable to accept connection)";
        case  4: return "CONNECT_BAD_CREDENTIALS (bad username or JWT)";
        case  5: return "CONNECT_UNAUTHORIZED (JWT rejected by ACL/auth)";
        default: return "UNKNOWN";
    }
}
}  // namespace

void MqttClient::staticCallback(char* topic, uint8_t* payload, unsigned int length) {
    // Dispatch to the matching per-topic handler. PubSubClient delivers every
    // subscribed topic through the same callback, so we have to demux here.
    if (mqttClient.firmwareTopic.length() &&
        mqttClient.firmwareHandler &&
        mqttClient.firmwareTopic == topic) {
        mqttClient.firmwareHandler(topic, payload, length);
        return;
    }
    if (mqttClient.configTopic.length() &&
        mqttClient.configHandler &&
        mqttClient.configTopic == topic) {
        mqttClient.configHandler(topic, payload, length);
        return;
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

    Serial.printf("[MQTT] begin: broker=%s:%d jwt_len=%u jwt_prefix=%s tls=plain\n",
                  broker.c_str(), port,
                  (unsigned)jwtToken.length(),
                  jwtToken.length() >= 8 ? jwtToken.substring(0, 8).c_str() : "(short)");

    return connect();
}

void MqttClient::setTopicPrefix(const char* mac) {
    // Default prefix matches the OMS subscriber contract:
    //   forgekey/<bare-mac>/people_counter[/occupancy|/firmware|/config]
    // (No leading slash. The OMS-side subscriber listens on
    // forgekey/<mac>/... — see backend/forgekey/utils.py.)
    topicPrefix = String("forgekey/") + mac + "/people_counter";
    if (occupancyTopic.length() == 0) {
        occupancyTopic = topicPrefix + "/occupancy";
    }
    Serial.printf("[MQTT] setTopicPrefix: prefix=%s default_occupancy=%s\n",
                  topicPrefix.c_str(), occupancyTopic.c_str());
}

void MqttClient::setOccupancyTopic(const char* topic) {
    if (topic && *topic) {
        Serial.printf("[MQTT] setOccupancyTopic: override '%s' -> '%s'\n",
                      occupancyTopic.c_str(), topic);
        occupancyTopic = topic;
    } else {
        Serial.println("[MQTT] setOccupancyTopic: empty value ignored, keeping default");
    }
}

void MqttClient::setFirmwareTopic(const char* topic) {
    if (topic && *topic) {
        Serial.printf("[MQTT] setFirmwareTopic: '%s' -> '%s'\n",
                      firmwareTopic.c_str(), topic);
        firmwareTopic = topic;
        if (isConnected()) {
            client->subscribe(firmwareTopic.c_str());
        }
        // Default the status topic alongside the dispatch topic. Conventional
        // shape: append "/status" to the dispatch topic so OMS can subscribe
        // to a single wildcard like forgekey/+/+/firmware/status. An explicit
        // setFirmwareStatusTopic() afterwards still wins.
        if (firmwareStatusTopic.length() == 0) {
            firmwareStatusTopic = firmwareTopic + "/status";
            Serial.printf("[MQTT] firmwareStatusTopic defaulted: '%s'\n",
                          firmwareStatusTopic.c_str());
        }
    } else {
        Serial.println("[MQTT] setFirmwareTopic: empty value ignored");
    }
}

void MqttClient::setFirmwareStatusTopic(const char* topic) {
    if (topic && *topic) {
        Serial.printf("[MQTT] setFirmwareStatusTopic: '%s' -> '%s'\n",
                      firmwareStatusTopic.c_str(), topic);
        firmwareStatusTopic = topic;
    } else {
        Serial.println("[MQTT] setFirmwareStatusTopic: empty value ignored");
    }
}

void MqttClient::setConfigTopic(const char* topic) {
    if (topic && *topic) {
        Serial.printf("[MQTT] setConfigTopic: '%s' -> '%s'\n",
                      configTopic.c_str(), topic);
        configTopic = topic;
        if (isConnected() && configHandler) {
            client->subscribe(configTopic.c_str());
        }
    } else {
        Serial.println("[MQTT] setConfigTopic: empty value ignored");
    }
}

bool MqttClient::connect() {
    if (!client) {
        Serial.println("[MQTT] connect: no client (begin() not called)");
        return false;
    }

    String clientId = "ForgeKey_" + String((uint32_t)ESP.getEfuseMac(), HEX);
    Serial.printf("[MQTT] connect: broker=%s:%d client_id=%s username=forgemqtt jwt_len=%u\n",
                  broker.c_str(), port, clientId.c_str(),
                  (unsigned)jwtToken.length());

    if (client->connect(clientId.c_str(), "forgemqtt", jwtToken.c_str())) {
        Serial.printf("[MQTT] connected: state=%d (%s)\n",
                      client->state(), mqttStateName(client->state()));
        resubscribeAll();
        return true;
    }

    int st = client->state();
    Serial.printf("[MQTT] connect FAILED: rc=%d (%s)\n", st, mqttStateName(st));
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
    if (!client) {
        Serial.println("[MQTT] publishOccupancy FAILED: no client (begin() not called)");
        return false;
    }
    if (!client->connected()) {
        int st = client->state();
        Serial.printf("[MQTT] publishOccupancy FAILED: not connected (state=%d %s); attempting reconnect\n",
                      st, mqttStateName(st));
        if (millis() - lastReconnectAttempt > 5000) {
            connect();
            lastReconnectAttempt = millis();
        }
        return false;
    }

    if (occupancyTopic.length() == 0) {
        Serial.println("[MQTT] publishOccupancy FAILED: occupancyTopic is empty "
                       "(setTopicPrefix not called and no override)");
        return false;
    }

    String payload = "{\"count\":" + String(count) +
                    ",\"timestamp\":" + String(millis()) + "}";

    bool result = client->publish(occupancyTopic.c_str(), payload.c_str());
    if (result) {
        Serial.printf("[MQTT] publish OK: topic=%s payload=%s\n",
                      occupancyTopic.c_str(), payload.c_str());
    } else {
        int st = client->state();
        Serial.printf("[MQTT] publish FAILED: topic=%s payload_len=%u state=%d (%s) "
                      "buffer_size=1024 — likely payload too large or socket closed\n",
                      occupancyTopic.c_str(), (unsigned)payload.length(),
                      st, mqttStateName(st));
    }

    return result;
}

bool MqttClient::publishFirmwareStatus(const char* state,
                                       const char* version,
                                       int progress,
                                       const char* error) {
    if (!client || !client->connected()) {
        // Best-effort; skip silently if offline. The OTA path must not block
        // on status publishing.
        return false;
    }
    if (firmwareStatusTopic.length() == 0) {
        Serial.println("[MQTT] publishFirmwareStatus: no status topic set, skipping");
        return false;
    }

    String payload = "{\"state\":\"";
    payload += (state ? state : "");
    payload += "\"";
    if (version && *version) {
        payload += ",\"version\":\"";
        payload += version;
        payload += "\"";
    }
    if (progress >= 0 && progress <= 100) {
        payload += ",\"progress\":";
        payload += String(progress);
    }
    if (error && *error) {
        payload += ",\"error\":\"";
        payload += error;
        payload += "\"";
    }
    payload += ",\"ts\":";
    payload += String(millis());
    payload += "}";

    bool ok = client->publish(firmwareStatusTopic.c_str(), payload.c_str());
    Serial.printf("[MQTT] publishFirmwareStatus: topic=%s payload=%s ok=%d\n",
                  firmwareStatusTopic.c_str(), payload.c_str(), (int)ok);
    return ok;
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
