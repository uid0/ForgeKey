#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <Client.h>
#include <functional>

// Schema contract identifiers shared by firmware publishers and OMS validators.
// Increment the suffix only for breaking schema changes; additive optional
// fields remain on the same schema version.
#define FORGEKEY_SCHEMA_STATUS_V1 "forgekey.status.v1"
#define FORGEKEY_SCHEMA_HEALTH_V1 "forgekey.health.v1"
#define FORGEKEY_SCHEMA_COMMAND_ACK_V1 "forgekey.command_ack.v1"
#define FORGEKEY_SCHEMA_OTA_STATUS_V1 "forgekey.ota_status.v1"
#define FORGEKEY_SCHEMA_OCCUPANCY_V1 "forgekey.occupancy.v1"
#define FORGEKEY_SCHEMA_TEMPERATURE_V1 "forgekey.temperature.v1"
#define FORGEKEY_SCHEMA_LOCK_STATUS_V1 "forgekey.lock_status.v1"
#define FORGEKEY_SCHEMA_EPAPER_V1 "forgekey.epaper.v1"
#define FORGEKEY_SCHEMA_BLE_V1 "forgekey.ble.v1"
#define FORGEKEY_SCHEMA_DIAGNOSTICS_V1 "forgekey.diagnostics.v1"
#define FORGEKEY_SCHEMA_ACCESS_REQUEST_V1 "forgekey.access_request.v1"

class MqttClient {
public:
    using MessageHandler = std::function<void(const char* topic,
                                              const uint8_t* payload,
                                              unsigned int length)>;

    bool begin(const char* broker, int port,
               const char* clientCertificatePem,
               const char* clientPrivateKeyPem,
               bool useTls = false);
    void setTopicPrefix(const char* macAddress);

    // Override the per-device topics returned by OMS at enrollment.
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
    // than handed back at enrollment so operators can address a device
    // by MAC alone before OMS knows its id.
    void setCommandTopic(const char* topic);
    void setStatusTopic(const char* topic);
    void setLogTopic(const char* topic);
    void setStateTopic(const char* topic);

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
    // Force a fresh firmware-topic subscription so brokers that deliver the
    // latest retained dispatch on SUBSCRIBE can be polled during the
    // post-restart rapid OTA check window.
    bool refreshFirmwareSubscription();
    bool subscribeConfig(MessageHandler handler);
    bool subscribeCommand(MessageHandler handler);
    // Publish blink on/off transition on statusTopic. Payload: {"blink":"on"}
    // or {"blink":"off"}. Best-effort; returns false if topic unset or socket
    // closed. Caller should still update local state regardless.
    bool publishBlinkStatus(bool on);
    // Publish an arbitrary JSON payload on statusTopic. Used for command
    // acks and operator-visible state echoes. Best-effort.
    bool publishStatus(const char* jsonPayload);
    // Queue a status payload for retry with exponential backoff. Critical
    // entries are persisted to NVS until PubSubClient accepts the publish.
    bool enqueueStatus(const char* jsonPayload, bool critical = true);
    // Publish the retained device state JSON on stateTopic so subscribers can
    // immediately see the device's last known online/offline state.
    bool publishStateJson(const char* jsonPayload);
    // Publish one structured device log event on logTopic. Payload:
    //   {"timestamp":1234,"level":"INFO","tag":"MAIN","message":"..."}
    // Best-effort and intentionally silent on failure to avoid log storms.
    bool publishLog(unsigned long timestampMs,
                    const char* level,
                    const char* tag,
                    const char* message);

    // BLE-specific publish helpers. Each builds a topic from the MAC prefix
    // and publishes the JSON payload. Best-effort (returns false if not
    // connected or topic unset).
    // Topics: forgekey/<mac>/ble/devices
    bool publishBleDevices(const char* jsonPayload);
    // Topics: forgekey/<mac>/ble/beacons
    bool publishBleBeacons(const char* jsonPayload);
    // Topics: forgekey/<mac>/ble/peers
    bool publishBlePeers(const char* jsonPayload);
    // Topics: forgekey/<mac>/ble/equipment
    bool publishEquipmentEvent(const char* jsonPayload);

    // Publish a badge_reader access-request event on
    // forgekey/<mac>/access/request. The device is a pure sensor: it emits the
    // credential, OMS authorizes and drives relay/lock/indicator. The contract
    // calls for QoS 1; PubSubClient only does QoS 0, so this publishes best-
    // effort and on failure enqueues to the NVS-persisted critical retry queue
    // (retain=false — an access request is an event, never retained state).
    bool publishAccessRequest(const char* jsonPayload);

    bool isConnected();
    void loop();
    bool restart();
    void end();

    // Diagnostic accessors. lastSuccessfulPublishMs() returns 0 if nothing
    // has ever been published successfully (so disconnect logging can say
    // "never" instead of a misleading age).
    unsigned long lastSuccessfulPublishMs() const { return lastPublishMs; }
    int lastConnectRc() const { return lastConnectState; }
    bool probeReachability(unsigned long timeoutMs = 5000);

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
    String logTopic;            // device logs (publish)
    String stateTopic;          // retained device state topic (publish + LWT)
    String capabilitiesTopic;   // capability announcement topic (publish)
    String bleDevicesTopic;     // BLE scan results (publish)
    String bleBeaconsTopic;     // beacon announcement (publish)
    String blePeersTopic;       // nearby ForgeKey peers (publish)
    String bleEquipmentTopic;   // equipment detect/lost events (publish)
    String accessRequestTopic;  // badge_reader access-request events (publish)
    String clientCertificatePem;
    String clientPrivateKeyPem;
    String broker;
    IPAddress brokerIp;
    bool brokerIpResolved = false;
    int port = 1883;
    unsigned long lastReconnectAttempt = 0;
    uint8_t reconnectFailures = 0;         // consecutive reconnect failures -> backoff
    unsigned long lastPublishMs = 0;       // millis() of last publish() == true
    int lastConnectState = 0;              // PubSubClient state after last connect attempt
    MessageHandler firmwareHandler;
    MessageHandler configHandler;
    MessageHandler commandHandler;

    struct OutboundMessage {
        String topic;
        String payload;
        bool retain = false;
        bool critical = false;
        unsigned long nextAttemptMs = 0;
        uint8_t attempts = 0;
    };
    static constexpr size_t kOutboundQueueSize = 8;
    OutboundMessage outboundQueue[kOutboundQueueSize];
    size_t outboundQueueCount = 0;

    bool connect();
    void resubscribeAll();
    bool publishImmediate(const char* topic, const char* payload, bool retain);
    bool enqueueOutbound(const char* topic, const char* payload, bool retain, bool critical);
    void serviceOutboundQueue();
    void persistCriticalQueue();
    void loadCriticalQueue();
    static void staticCallback(char* topic, uint8_t* payload, unsigned int length);
};

extern MqttClient mqttClient;

#endif
