#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <PubSubClient.h>
#include <WiFiClient.h>
#include <Client.h>
#include <functional>

class MqttClient {
public:
    using MessageHandler = std::function<void(const char* topic,
                                              const uint8_t* payload,
                                              unsigned int length)>;

    bool begin(const char* broker, int port, const char* jwtToken,
               bool useTls = false);
    void setTopicPrefix(const char* macAddress);

    // Override the per-device topics returned by OMS at registration.
    // If empty, defaults derived from the MAC are used.
    void setOccupancyTopic(const char* topic);
    void setReadingTopic(const char* topic);
    void setFirmwareTopic(const char* topic);
    void setFirmwareStatusTopic(const char* topic);
    void setConfigTopic(const char* topic);
    // Per-device control plane: commands flow in on commandTopic
    // (forgekey/<mac>/command), and operator-visible state changes
    // (e.g. blink on/off) are published on statusTopic
    // (forgekey/<mac>/status). Topics are MAC-derived in setup() rather
    // than handed back at registration so operators can address a device
    // by MAC alone before OMS knows its id.
    void setCommandTopic(const char* topic);
    void setStatusTopic(const char* topic);

    bool publishOccupancy(int count);
    // Publish a temperature/humidity reading on the device's reading topic.
    // Topic shape: forgekey/<mac>/temperature_sensor/reading. Payload:
    //   {"tempC": 21.4, "humidity": 47.1, "timestamp": <millis>}
    bool publishTemperature(float tempC, float humidity);
    // Publish the device's capability announcement on
    // forgekey/<mac>/capabilities with retained=true. PubSubClient does not
    // support QoS 1 publish (only QoS 0); retained=true gives equivalent
    // late-subscriber semantics for this one-shot announcement.
    bool publishCapabilities(const char* jsonPayload);
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
    bool subscribeCommand(MessageHandler handler);
    // Publish blink on/off transition on statusTopic. Payload: {"blink":"on"}
    // or {"blink":"off"}. Best-effort; returns false if topic unset or socket
    // closed. Caller should still update local state regardless.
    bool publishBlinkStatus(bool on);
    // Publish an arbitrary JSON payload on statusTopic. Used for command
    // acks and operator-visible state echoes. Best-effort.
    bool publishStatus(const char* jsonPayload);
    bool isConnected();
    void loop();
    void end();

    // Diagnostic accessors. lastSuccessfulPublishMs() returns 0 if nothing
    // has ever been published successfully (so disconnect logging can say
    // "never" instead of a misleading age).
    unsigned long lastSuccessfulPublishMs() const { return lastPublishMs; }
    int lastConnectRc() const { return lastConnectState; }

private:
    // Net transport: either a plain WiFiClient or a WiFiClientSecure depending
    // on whether the broker speaks TLS. Heap-allocated so we can swap on each
    // begin() without slicing.
    Client* netClient = nullptr;
    bool useTls = false;
    PubSubClient* client = nullptr;
    String topicPrefix;
    String occupancyTopic;  // resolved publish topic (people/door counter builds)
    String readingTopic;    // resolved publish topic (temperature sensor builds)
    String firmwareTopic;       // OTA dispatch topic (subscribe)
    String firmwareStatusTopic; // OTA progress topic (publish)
    String configTopic;         // credential-rotation / config command topic
    String commandTopic;        // per-device control commands (subscribe)
    String statusTopic;         // operator-visible state changes (publish)
    String capabilitiesTopic;   // capability announcement topic (publish)
    String jwtToken;
    String broker;
    int port = 1883;
    unsigned long lastReconnectAttempt = 0;
    unsigned long lastPublishMs = 0;       // millis() of last publish() == true
    int lastConnectState = 0;              // PubSubClient state after last connect attempt
    MessageHandler firmwareHandler;
    MessageHandler configHandler;
    MessageHandler commandHandler;

    bool connect();
    void resubscribeAll();
    static void staticCallback(char* topic, uint8_t* payload, unsigned int length);
};

extern MqttClient mqttClient;

#endif
