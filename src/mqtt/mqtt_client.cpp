#include "mqtt_client.h"
#include "Arduino.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include "security/oms_ca.h"

// Compile-time topic kind. The OMS subscriber listens on
// forgekey/<mac>/<kind>/... — kind is a string segment that identifies the
// device's capability bucket. Override via -DFORGEKEY_MQTT_TOPIC_KIND in the
// build env (e.g. "temperature_sensor", "door_counter").
#ifndef FORGEKEY_MQTT_TOPIC_KIND
#define FORGEKEY_MQTT_TOPIC_KIND "people_counter"
#endif

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
        case  4: return "CONNECT_BAD_CREDENTIALS (broker rejected credentials)";
        case  5: return "CONNECT_UNAUTHORIZED (broker rejected authz/policy)";
        default: return "UNKNOWN";
    }
}

String escapeJsonString(const char* value) {
    String out;
    if (!value) return out;

    while (*value) {
        const char ch = *value++;
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)ch < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)ch);
                    out += buf;
                } else {
                    out += ch;
                }
                break;
        }
    }
    return out;
}

String defaultStateTopic() {
    uint64_t chipMac = ESP.getEfuseMac();
    char macBuf[13];
    snprintf(macBuf, sizeof(macBuf), "%012llx",
             static_cast<unsigned long long>(chipMac));
    return String("forgekey/") + macBuf + "/state";
}

String buildStatePayload(bool online, const char* ip, const char* reason) {
    String payload;
    payload.reserve(96);
    payload += "{\"online\":";
    payload += online ? "true" : "false";
    if (ip && *ip) {
        payload += ",\"ip\":\"";
        payload += escapeJsonString(ip);
        payload += "\"";
    }
    if (reason && *reason) {
        payload += ",\"reason\":\"";
        payload += escapeJsonString(reason);
        payload += "\"";
    }
    payload += "}";
    return payload;
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
    if (mqttClient.commandTopic.length() &&
        mqttClient.commandHandler &&
        mqttClient.commandTopic == topic) {
        mqttClient.commandHandler(topic, payload, length);
        return;
    }
    if (mqttClient.lockTopic.length() &&
        mqttClient.lockHandler &&
        mqttClient.lockTopic == topic) {
        mqttClient.lockHandler(topic, payload, length);
        return;
    }
}

bool MqttClient::begin(const char* brokerHost, int portNum,
                       const char* clientCertPem,
                       const char* clientKeyPem,
                       bool useTlsArg) {
    if (client) { delete client; client = nullptr; }
    if (netClient) { delete netClient; netClient = nullptr; }

    useTls = useTlsArg;
    if (useTls) {
        // CA pinning: reuse the OMS HTTPS root. Brokers fronted by a different
        // CA will fail TLS handshake — track that as a separate bead if it
        // becomes a real deployment.
        WiFiClientSecure* secure = new WiFiClientSecure();
        secure->setCACert(kOmsCaPem);
        if (clientCertPem && clientKeyPem &&
            strlen(clientCertPem) > 0 && strlen(clientKeyPem) > 0) {
            secure->setCertificate(clientCertPem);
            secure->setPrivateKey(clientKeyPem);
        } else {
            Serial.println("[MQTT] begin: WARNING mTLS requested but client cert/key missing");
        }
        netClient = secure;
    } else {
        netClient = new WiFiClient();
    }

    client = new PubSubClient(*netClient);
    broker = brokerHost;
    port = portNum;
    client->setCallback(MqttClient::staticCallback);
    client->setBufferSize(1024);  // larger payloads for OTA dispatch JSON
    clientCertificatePem = clientCertPem ? clientCertPem : "";
    clientPrivateKeyPem = clientKeyPem ? clientKeyPem : "";

    if (broker.length()) {
        // Always configure PubSubClient with the hostname. WiFiClientSecure
        // needs the original host name for SNI and certificate name checks;
        // passing only the resolved IP can make a valid cert fail
        // verification because it no longer matches the peer identity.
        client->setServer(broker.c_str(), port);
        IPAddress resolvedIp;
        if (WiFi.hostByName(broker.c_str(), resolvedIp)) {
            brokerIp = resolvedIp;
            brokerIpResolved = true;
            Serial.printf("[MQTT] broker host resolved: %s -> %s\n",
                          broker.c_str(), brokerIp.toString().c_str());
        } else {
            Serial.printf("[MQTT] broker host DNS lookup failed: %s\n",
                          broker.c_str());
        }
    }

    // auth_method=mtls: device identity is presented during the TLS
    // handshake using the provisioned client certificate and private key.
    Serial.printf("[MQTT] begin: broker=%s:%d auth_method=mtls "
                  "client_cert_len=%u client_key_len=%u tls=%s\n",
                  broker.c_str(), port,
                  (unsigned)clientCertificatePem.length(),
                  (unsigned)clientPrivateKeyPem.length(),
                  useTls ? "on" : "plain");
    if (useTls && (clientCertificatePem.length() == 0 || clientPrivateKeyPem.length() == 0)) {
        Serial.println("[MQTT] begin: WARNING mTLS identity is empty — broker will reject the session");
    }

    return connect();
}

void MqttClient::setTopicPrefix(const char* mac) {
    // Default prefix matches the OMS subscriber contract:
    //   forgekey/<bare-mac>/<kind>[/occupancy|/reading|/firmware|/config]
    // (No leading slash. The OMS-side subscriber listens on
    // forgekey/<mac>/... — see backend/forgekey/utils.py.) The <kind>
    // segment is selected at compile time via FORGEKEY_MQTT_TOPIC_KIND.
    topicPrefix = String("forgekey/") + mac + "/" + FORGEKEY_MQTT_TOPIC_KIND;
    if (occupancyTopic.length() == 0) {
        occupancyTopic = topicPrefix + "/occupancy";
    }
    if (readingTopic.length() == 0) {
        readingTopic = topicPrefix + "/reading";
    }
    // Capability announcement topic lives directly under the MAC, not the
    // kind segment, so a single device with multiple capabilities still has
    // one announcement endpoint (forgekey/<mac>/capabilities).
    capabilitiesTopic = String("forgekey/") + mac + "/capabilities";

    // BLE topics also live directly under the MAC (not under the kind
    // segment) so OMS can subscribe to all BLE data with a single pattern.
    if (bleDevicesTopic.length() == 0) {
        bleDevicesTopic = String("forgekey/") + mac + "/ble/devices";
    }
    if (bleBeaconsTopic.length() == 0) {
        bleBeaconsTopic = String("forgekey/") + mac + "/ble/beacons";
    }
    if (blePeersTopic.length() == 0) {
        blePeersTopic = String("forgekey/") + mac + "/ble/peers";
    }
    if (bleEquipmentTopic.length() == 0) {
        bleEquipmentTopic = String("forgekey/") + mac + "/ble/equipment";
    }
    if (logTopic.length() == 0) {
        logTopic = String("forgekey/") + mac + "/logs";
    }
    if (stateTopic.length() == 0) {
        stateTopic = String("forgekey/") + mac + "/state";
    }

    Serial.printf("[MQTT] setTopicPrefix: prefix=%s default_occupancy=%s default_reading=%s capabilities=%s ble_devices=%s ble_beacons=%s ble_peers=%s ble_equipment=%s logs=%s state=%s\n",
                  topicPrefix.c_str(), occupancyTopic.c_str(),
                  readingTopic.c_str(), capabilitiesTopic.c_str(),
                  bleDevicesTopic.c_str(), bleBeaconsTopic.c_str(),
                  blePeersTopic.c_str(), bleEquipmentTopic.c_str(),
                  logTopic.c_str(), stateTopic.c_str());
}

bool MqttClient::publishCapabilities(const char* jsonPayload) {
    if (!client) {
        Serial.println("[MQTT] publishCapabilities FAILED: no client (begin() not called)");
        return false;
    }
    if (!client->connected()) {
        int st = client->state();
        Serial.printf("[MQTT] publishCapabilities FAILED: not connected (state=%d %s)\n",
                      st, mqttStateName(st));
        return false;
    }
    if (capabilitiesTopic.length() == 0) {
        Serial.println("[MQTT] publishCapabilities FAILED: capabilitiesTopic is empty "
                       "(setTopicPrefix not called)");
        return false;
    }
    if (!jsonPayload) jsonPayload = "";

    size_t len = strlen(jsonPayload);
    bool ok = client->publish(capabilitiesTopic.c_str(),
                              reinterpret_cast<const uint8_t*>(jsonPayload),
                              len,
                              true /* retained */);
    if (ok) {
        lastPublishMs = millis();
        Serial.printf("[MQTT] publishCapabilities OK: topic=%s retained=true payload_len=%u payload=%s\n",
                      capabilitiesTopic.c_str(), (unsigned)len, jsonPayload);
    } else {
        int st = client->state();
        Serial.printf("[MQTT] publishCapabilities FAILED: topic=%s payload_len=%u state=%d (%s)\n",
                      capabilitiesTopic.c_str(), (unsigned)len, st, mqttStateName(st));
    }
    return ok;
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

void MqttClient::setReadingTopic(const char* topic) {
    if (topic && *topic) {
        Serial.printf("[MQTT] setReadingTopic: override '%s' -> '%s'\n",
                      readingTopic.c_str(), topic);
        readingTopic = topic;
    } else {
        Serial.println("[MQTT] setReadingTopic: empty value ignored, keeping default");
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

void MqttClient::setCommandTopic(const char* topic) {
    if (topic && *topic) {
        Serial.printf("[MQTT] setCommandTopic: '%s' -> '%s'\n",
                      commandTopic.c_str(), topic);
        commandTopic = topic;
        if (isConnected() && commandHandler) {
            client->subscribe(commandTopic.c_str());
        }
    } else {
        Serial.println("[MQTT] setCommandTopic: empty value ignored");
    }
}

void MqttClient::setStatusTopic(const char* topic) {
    if (topic && *topic) {
        Serial.printf("[MQTT] setStatusTopic: '%s' -> '%s'\n",
                      statusTopic.c_str(), topic);
        statusTopic = topic;
    } else {
        Serial.println("[MQTT] setStatusTopic: empty value ignored");
    }
}

void MqttClient::setLogTopic(const char* topic) {
    if (topic && *topic) {
        Serial.printf("[MQTT] setLogTopic: '%s' -> '%s'\n",
                      logTopic.c_str(), topic);
        logTopic = topic;
    } else {
        Serial.println("[MQTT] setLogTopic: empty value ignored");
    }
}

void MqttClient::setStateTopic(const char* topic) {
    if (topic && *topic) {
        Serial.printf("[MQTT] setStateTopic: '%s' -> '%s'\n",
                      stateTopic.c_str(), topic);
        stateTopic = topic;
    } else {
        Serial.println("[MQTT] setStateTopic: empty value ignored");
    }
}

bool MqttClient::connect() {
    if (!client) {
        Serial.println("[MQTT] connect: no client (begin() not called)");
        return false;
    }

    String clientId = "ForgeKey_" + String((uint32_t)ESP.getEfuseMac(), HEX);
    if (stateTopic.length() == 0) {
        stateTopic = defaultStateTopic();
        Serial.printf("[MQTT] stateTopic defaulted during connect: %s\n",
                      stateTopic.c_str());
    }
    Serial.printf("[MQTT] connect: broker=%s:%d client_id=%s client_cert_len=%u\n",
                  broker.c_str(), port, clientId.c_str(),
                  (unsigned)clientCertificatePem.length());

    if (!brokerIpResolved && broker.length()) {
        IPAddress resolvedIp;
        for (int retry = 0; retry < 3; retry++) {
            if (WiFi.hostByName(broker.c_str(), resolvedIp)) {
                brokerIp = resolvedIp;
                brokerIpResolved = true;
                Serial.printf("[MQTT] broker host resolved on connect: %s -> %s\n",
                              broker.c_str(), brokerIp.toString().c_str());
                break;
            }
            delay(500);
        }
    }

    static const char* kUnexpectedDisconnectPayload =
        "{\"online\":false,\"reason\":\"unexpected_disconnect\"}";
    if (client->connect(clientId.c_str(), nullptr, nullptr,
                        stateTopic.c_str(), 0, true,
                        kUnexpectedDisconnectPayload)) {
        lastConnectState = client->state();
        Serial.printf("[MQTT] connected: state=%d (%s)\n",
                      lastConnectState, mqttStateName(lastConnectState));
        String ip = WiFi.localIP().toString();
        String birthPayload = buildStatePayload(true, ip.c_str(), nullptr);
        publishStateJson(birthPayload.c_str());
        resubscribeAll();
        return true;
    }

    lastConnectState = client->state();
    Serial.printf("[MQTT] connect FAILED: rc=%d (%s)\n",
                  lastConnectState, mqttStateName(lastConnectState));
    if (lastConnectState == 4 || lastConnectState == 5) {
        Serial.println("[MQTT] connect FAILED: broker rejected the client certificate "
                       "or the broker-side mTLS policy does not match this device.");
    }
    return false;
}

void MqttClient::resubscribeAll() {
    // PubSubClient's subscribe() returns true iff the SUBSCRIBE packet was
    // written to the socket — it does NOT confirm a SUBACK from the broker.
    // A "true" here only means "we tried"; if you don't see commands flow,
    // suspect ACL on the broker side.
    if (firmwareTopic.length() && firmwareHandler) {
        bool ok = client->subscribe(firmwareTopic.c_str());
        Serial.printf("[MQTT] subscribe firmware topic=%s ok=%d (write to socket; SUBACK not awaited)\n",
                      firmwareTopic.c_str(), (int)ok);
    } else {
        Serial.printf("[MQTT] subscribe firmware SKIPPED: topic_len=%u handler=%s\n",
                      (unsigned)firmwareTopic.length(),
                      firmwareHandler ? "set" : "(null)");
    }
    if (configTopic.length() && configHandler) {
        bool ok = client->subscribe(configTopic.c_str());
        Serial.printf("[MQTT] subscribe config topic=%s ok=%d (write to socket; SUBACK not awaited)\n",
                      configTopic.c_str(), (int)ok);
    } else {
        Serial.printf("[MQTT] subscribe config SKIPPED: topic_len=%u handler=%s\n",
                      (unsigned)configTopic.length(),
                      configHandler ? "set" : "(null)");
    }
    if (commandTopic.length() && commandHandler) {
        bool ok = client->subscribe(commandTopic.c_str());
        Serial.printf("[MQTT] subscribe command topic=%s ok=%d (write to socket; SUBACK not awaited)\n",
                      commandTopic.c_str(), (int)ok);
    } else {
        Serial.printf("[MQTT] subscribe command SKIPPED: topic_len=%u handler=%s\n",
                      (unsigned)commandTopic.length(),
                      commandHandler ? "set" : "(null)");
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

    // PubSubClient::publish() with the (topic, payload) signature defaults to
    // QoS 0 / retain=false. There is no PUBACK at QoS 0; "ok" only means
    // "we wrote it to the socket" — broker delivery is not confirmed.
    bool result = client->publish(occupancyTopic.c_str(), payload.c_str());
    if (result) {
        lastPublishMs = millis();
        Serial.printf("[MQTT] publish OK: topic=%s qos=0 payload_len=%u payload=%s\n",
                      occupancyTopic.c_str(),
                      (unsigned)payload.length(), payload.c_str());
    } else {
        int st = client->state();
        Serial.printf("[MQTT] publish FAILED: topic=%s qos=0 payload_len=%u state=%d (%s) "
                      "buffer_size=1024 — likely payload too large or socket closed\n",
                      occupancyTopic.c_str(), (unsigned)payload.length(),
                      st, mqttStateName(st));
    }

    return result;
}

bool MqttClient::publishTemperature(float tempC, float humidity) {
    if (!client) {
        Serial.println("[MQTT] publishTemperature FAILED: no client (begin() not called)");
        return false;
    }
    if (!client->connected()) {
        int st = client->state();
        Serial.printf("[MQTT] publishTemperature FAILED: not connected (state=%d %s); attempting reconnect\n",
                      st, mqttStateName(st));
        if (millis() - lastReconnectAttempt > 5000) {
            connect();
            lastReconnectAttempt = millis();
        }
        return false;
    }

    if (readingTopic.length() == 0) {
        Serial.println("[MQTT] publishTemperature FAILED: readingTopic is empty "
                       "(setTopicPrefix not called and no override)");
        return false;
    }

    char tBuf[16], hBuf[16];
    dtostrf(tempC, 0, 2, tBuf);
    dtostrf(humidity, 0, 2, hBuf);
    String payload = "{\"tempC\":";
    payload += tBuf;
    payload += ",\"humidity\":";
    payload += hBuf;
    payload += ",\"timestamp\":";
    payload += String(millis());
    payload += "}";

    bool result = client->publish(readingTopic.c_str(), payload.c_str());
    if (result) {
        lastPublishMs = millis();
        Serial.printf("[MQTT] publish OK: topic=%s qos=0 payload_len=%u payload=%s\n",
                      readingTopic.c_str(),
                      (unsigned)payload.length(), payload.c_str());
    } else {
        int st = client->state();
        Serial.printf("[MQTT] publish FAILED: topic=%s qos=0 payload_len=%u state=%d (%s)\n",
                      readingTopic.c_str(), (unsigned)payload.length(),
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
    if (ok) lastPublishMs = millis();
    Serial.printf("[MQTT] publishFirmwareStatus: topic=%s qos=0 payload=%s ok=%d\n",
                  firmwareStatusTopic.c_str(), payload.c_str(), (int)ok);
    return ok;
}

bool MqttClient::subscribeFirmware(MessageHandler handler) {
    firmwareHandler = handler;
    if (firmwareTopic.length() == 0 || !client) {
        Serial.printf("[MQTT] subscribeFirmware DEFERRED: topic_len=%u client=%s\n",
                      (unsigned)firmwareTopic.length(), client ? "ok" : "(null)");
        return false;
    }
    if (!client->connected()) {
        Serial.printf("[MQTT] subscribeFirmware DEFERRED: not connected (state=%d %s); "
                      "will resubscribe on next connect\n",
                      client->state(), mqttStateName(client->state()));
        return false;
    }
    bool ok = client->subscribe(firmwareTopic.c_str());
    Serial.printf("[MQTT] subscribeFirmware: topic=%s ok=%d\n",
                  firmwareTopic.c_str(), (int)ok);
    return ok;
}

bool MqttClient::subscribeCommand(MessageHandler handler) {
    commandHandler = handler;
    if (commandTopic.length() == 0 || !client) {
        Serial.printf("[MQTT] subscribeCommand DEFERRED: topic_len=%u client=%s\n",
                      (unsigned)commandTopic.length(), client ? "ok" : "(null)");
        return false;
    }
    if (!client->connected()) {
        Serial.printf("[MQTT] subscribeCommand DEFERRED: not connected (state=%d %s); "
                      "will resubscribe on next connect\n",
                      client->state(), mqttStateName(client->state()));
        return false;
    }
    bool ok = client->subscribe(commandTopic.c_str());
    Serial.printf("[MQTT] subscribeCommand: topic=%s ok=%d\n",
                  commandTopic.c_str(), (int)ok);
    return ok;
}

bool MqttClient::publishBlinkStatus(bool on) {
    return publishStatus(on ? "{\"blink\":\"on\"}" : "{\"blink\":\"off\"}");
}

bool MqttClient::publishStatus(const char* jsonPayload) {
    if (!client || !client->connected()) return false;
    if (statusTopic.length() == 0) {
        Serial.println("[MQTT] publishStatus: no status topic set, skipping");
        return false;
    }
    if (!jsonPayload) jsonPayload = "{}";
    bool ok = client->publish(statusTopic.c_str(), jsonPayload);
    if (ok) lastPublishMs = millis();
    Serial.printf("[MQTT] publishStatus: topic=%s payload=%s ok=%d\n",
                  statusTopic.c_str(), jsonPayload, (int)ok);
    return ok;
}

bool MqttClient::publishStateJson(const char* jsonPayload) {
    if (!client || !client->connected()) return false;
    if (stateTopic.length() == 0) {
        Serial.println("[MQTT] publishStateJson: no state topic set, skipping");
        return false;
    }
    if (!jsonPayload) jsonPayload = "{}";
    bool ok = client->publish(stateTopic.c_str(), jsonPayload, true);
    if (ok) lastPublishMs = millis();
    Serial.printf("[MQTT] publishStateJson: topic=%s retained=true payload=%s ok=%d\n",
                  stateTopic.c_str(), jsonPayload, (int)ok);
    return ok;
}

bool MqttClient::publishLog(unsigned long timestampMs,
                            const char* level,
                            const char* tag,
                            const char* message) {
    if (!client || !client->connected()) return false;
    if (logTopic.length() == 0) return false;

    String payload;
    payload.reserve(256);
    payload += "{\"timestamp\":";
    payload += String(timestampMs);
    payload += ",\"level\":\"";
    payload += escapeJsonString(level ? level : "");
    payload += "\",\"tag\":\"";
    payload += escapeJsonString(tag ? tag : "");
    payload += "\",\"message\":\"";
    payload += escapeJsonString(message ? message : "");
    payload += "\"}";

    bool ok = client->publish(logTopic.c_str(), payload.c_str());
    if (ok) lastPublishMs = millis();
    return ok;
}

bool MqttClient::publishBleDevices(const char* jsonPayload) {
    if (!client || !client->connected()) return false;
    if (bleDevicesTopic.length() == 0) return false;
    if (!jsonPayload) jsonPayload = "{}";
    bool ok = client->publish(bleDevicesTopic.c_str(), jsonPayload);
    if (ok) lastPublishMs = millis();
    return ok;
}

bool MqttClient::publishBleBeacons(const char* jsonPayload) {
    if (!client || !client->connected()) return false;
    if (bleBeaconsTopic.length() == 0) return false;
    if (!jsonPayload) jsonPayload = "{}";
    bool ok = client->publish(bleBeaconsTopic.c_str(), jsonPayload);
    if (ok) lastPublishMs = millis();
    return ok;
}

bool MqttClient::publishBlePeers(const char* jsonPayload) {
    if (!client || !client->connected()) return false;
    if (blePeersTopic.length() == 0) return false;
    if (!jsonPayload) jsonPayload = "{}";
    bool ok = client->publish(blePeersTopic.c_str(), jsonPayload);
    if (ok) lastPublishMs = millis();
    return ok;
}

bool MqttClient::publishEquipmentEvent(const char* jsonPayload) {
    if (!client || !client->connected()) return false;
    if (bleEquipmentTopic.length() == 0) return false;
    if (!jsonPayload) jsonPayload = "{}";
    bool ok = client->publish(bleEquipmentTopic.c_str(), jsonPayload);
    if (ok) lastPublishMs = millis();
    return ok;
}

bool MqttClient::isConnected() {
    return client && client->connected();
}

void MqttClient::loop() {
    if (!client) return;
    
    // Process any pending messages/pings
    client->loop();
    
    // Reconnect if connection is lost (same 5s throttle as publish path)
    if (!client->connected()) {
        if (millis() - lastReconnectAttempt > 5000) {
            Serial.printf("[MQTT] loop: connection lost, attempting reconnect (last_state=%d %s)\n",
                          client->state(), mqttStateName(client->state()));
            connect();
            lastReconnectAttempt = millis();
        }
    }
}

bool MqttClient::subscribeConfig(MessageHandler handler) {
    configHandler = handler;
    if (configTopic.length() == 0 || !client) {
        Serial.printf("[MQTT] subscribeConfig DEFERRED: topic_len=%u client=%s\n",
                      (unsigned)configTopic.length(), client ? "ok" : "(null)");
        return false;
    }
    if (!client->connected()) {
        Serial.printf("[MQTT] subscribeConfig DEFERRED: not connected (state=%d %s); "
                      "will resubscribe on next connect\n",
                      client->state(), mqttStateName(client->state()));
        return false;
    }
    bool ok = client->subscribe(configTopic.c_str());
    Serial.printf("[MQTT] subscribeConfig: topic=%s ok=%d\n",
                  configTopic.c_str(), (int)ok);
    return ok;
}

void MqttClient::setLockTopic(const char* topic) {
    if (topic && *topic) {
        Serial.printf("[MQTT] setLockTopic: '%s'\n", topic);
        lockTopic = topic;
    } else {
        Serial.println("[MQTT] setLockTopic: empty value ignored");
    }
}

void MqttClient::subscribeLock(MessageHandler handler) {
    lockHandler = handler;
    if (lockTopic.length() == 0 || !client) {
        Serial.printf("[MQTT] subscribeLock DEFERRED: topic_len=%u client=%s\n",
                      (unsigned)lockTopic.length(), client ? "ok" : "(null)");
        return;
    }
    if (!client->connected()) {
        Serial.printf("[MQTT] subscribeLock DEFERRED: not connected (state=%d %s); "
                      "will resubscribe on next connect\n",
                      client->state(), mqttStateName(client->state()));
        return;
    }
    bool ok = client->subscribe(lockTopic.c_str());
    Serial.printf("[MQTT] subscribeLock: topic=%s ok=%d\n",
                  lockTopic.c_str(), (int)ok);
}

void MqttClient::end() {
    if (client) {
        if (client->connected()) {
            String offlinePayload =
                buildStatePayload(false, nullptr, "graceful_disconnect");
            publishStateJson(offlinePayload.c_str());
            client->disconnect();
        }
        delete client;
        client = nullptr;
    }
    if (netClient) {
        delete netClient;
        netClient = nullptr;
    }
}
