// Inter-device BLE relay: when MQTT is down, store messages in NVS and
// relay them to nearby ForgeKey devices via BLE GATT.

#ifndef FORGEKEY_DISABLE_BLE_RELAY

#include "../capability.h"
#include "ble_relay.h"

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEService.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include <nvs_flash.h>
#include <ArduinoJson.h>

#include "../../mqtt/mqtt_client.h"
#include "../../provisioning/device_config.h"

#ifndef BLE_RELAY_QUEUE_SIZE
#define BLE_RELAY_QUEUE_SIZE 20
#endif

#ifndef BLE_RELAY_PEER_TIMEOUT_MS
#define BLE_RELAY_PEER_TIMEOUT_MS 300000UL  // 5 minutes
#endif

#ifndef BLE_RELAY_MSG_TTL_MS
#define BLE_RELAY_MSG_TTL_MS 3600000UL  // 1 hour
#endif

// GATT service UUID
#define FORGEKEY_RELAY_SERVICE_UUID "B1B2C3D4-E5F6-7890-ABCD-EF1234567891"
#define FORGEKEY_RELAY_CHAR_UUID    "B1B2C3D4-E5F6-7890-ABCD-EF1234567892"

namespace BleRelay {

bool detectFn();
void setupFn();
void tickFn();

namespace {

bool g_active = false;
bool g_enabled = true;

// Peer table
struct Peer {
    char mac[13];
    int8_t lastRssi;
    unsigned long lastSeen;
    bool connected;
};

Peer g_peers[16];
int g_peerCount = 0;

// Message queue (ring buffer in memory)
struct RelayMsg {
    char src[13];
    char dst[13];
    char topic[64];
    char payload[256];
    unsigned long ts;
    bool delivered;
};

RelayMsg g_queue[BLE_RELAY_QUEUE_SIZE];
int g_queueHead = 0;
int g_queueCount = 0;

// BLE client for pushing messages to peers
BLEClient* g_bleClient = nullptr;
bool g_bleConnecting = false;

// Scan callback for detecting ForgeKey peers
class RelayScanCallback : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        String macStr = advertisedDevice.getAddress().toString().c_str();
        macStr.replace(":", "");
        macStr.toLowerCase();
        if (macStr.length() < 12) return;

        char mac[13];
        strncpy(mac, macStr.substring(0, 12).c_str(), 12);
        mac[12] = '\0';

        // Check if this is a ForgeKey beacon
        std::string uuidStr = advertisedDevice.getServiceUUID().toString();
        if (uuidStr != "A1B2C3D4-E5F6-7890-ABCD-EF1234567890") return;

        // Update or add peer
        int idx = -1;
        for (int i = 0; i < g_peerCount; i++) {
            if (strcmp(g_peers[i].mac, mac) == 0) {
                idx = i;
                break;
            }
        }
        if (idx < 0 && g_peerCount < 16) {
            idx = g_peerCount++;
            strncpy(g_peers[idx].mac, mac, 12);
            g_peers[idx].mac[12] = '\0';
        }
        if (idx >= 0) {
            g_peers[idx].lastRssi = advertisedDevice.getRSSI();
            g_peers[idx].lastSeen = millis();
            g_peers[idx].connected = false;
        }
    }
};

// Find oldest undelivered message destined for a specific peer
int findQueueEntry(const char* dstMac) {
    for (int i = 0; i < g_queueCount; i++) {
        int idx = (g_queueHead + i) % BLE_RELAY_QUEUE_SIZE;
        if (strcmp(g_queue[idx].dst, dstMac) == 0 && !g_queue[idx].delivered) {
            return idx;
        }
    }
    return -1;
}

// Push queued messages to a peer via BLE GATT
void pushToPeer(int peerIdx) {
    if (peerIdx < 0 || peerIdx >= g_peerCount) return;
    Peer* peer = &g_peers[peerIdx];
    if (peer->connected) return;

    // Find a message for this peer
    int msgIdx = findQueueEntry(peer->mac);
    if (msgIdx < 0) return;

    // Connect via BLE GATT client
    if (g_bleConnecting) return;
    g_bleConnecting = true;

    // Build MAC address string for BLEAddress (aa:bb:cc:dd:ee:ff format)
    String addrStr;
    for (int i = 0; i < 12; i++) {
        if (i > 0 && i % 2 == 0) addrStr += ":";
        addrStr += String(peer->mac[i]);
    }

    BLEAddress addr(addrStr.c_str());
    g_bleClient = BLEDevice::createClient();
    g_bleClient->setClientCallbacks(nullptr);
    g_bleClient->connect(addr);

    BLERemoteService* service = g_bleClient->getService(FORGEKEY_RELAY_SERVICE_UUID);
    if (!service) {
        g_bleConnecting = false;
        g_bleClient->disconnect();
        delete g_bleClient;
        g_bleClient = nullptr;
        return;
    }

    BLERemoteCharacteristic* charac = service->getCharacteristic(FORGEKEY_RELAY_CHAR_UUID);
    if (!charac) {
        g_bleConnecting = false;
        g_bleClient->disconnect();
        delete g_bleClient;
        g_bleClient = nullptr;
        return;
    }

    charac->writeValue((uint8_t*)g_queue[msgIdx].payload, strlen(g_queue[msgIdx].payload), false);
    g_queue[msgIdx].delivered = true;
    peer->connected = true;

    Serial.printf("[CAP/ble_relay] pushed message to %s via BLE GATT\n", peer->mac);

    g_bleConnecting = false;
    g_bleClient->disconnect();
    delete g_bleClient;
    g_bleClient = nullptr;
}

// Sync queued messages to MQTT on reconnect
void syncQueueToMqtt() {
    if (!mqttClient.isConnected()) return;

    for (int i = 0; i < g_queueCount; i++) {
        int idx = (g_queueHead + i) % BLE_RELAY_QUEUE_SIZE;
        if (g_queue[idx].delivered) continue;

        String topic = String("forgekey/") + g_queue[idx].dst + "/ble/relay";

        StaticJsonDocument<512> doc;
        doc["src"] = g_queue[idx].src;
        doc["topic"] = g_queue[idx].topic;
        doc["payload"] = g_queue[idx].payload;
        doc["ts"] = g_queue[idx].ts;
        doc["relay"] = true;

        String json;
        serializeJson(doc, json);

        if (mqttClient.publishStatus(json.c_str())) {
            g_queue[idx].delivered = true;
            Serial.printf("[CAP/ble_relay] synced message to MQTT: topic=%s\n", g_queue[idx].topic);
        }
    }
}

// Add message to queue (ring buffer)
void enqueueMessage(const char* src, const char* dst, const char* topic, const char* payload) {
    if (g_queueCount >= BLE_RELAY_QUEUE_SIZE) {
        int oldest = g_queueHead;
        g_queueHead = (g_queueHead + 1) % BLE_RELAY_QUEUE_SIZE;
        g_queueCount--;
    }

    int idx = (g_queueHead + g_queueCount) % BLE_RELAY_QUEUE_SIZE;
    strncpy(g_queue[idx].src, src, 12);
    g_queue[idx].src[12] = '\0';
    strncpy(g_queue[idx].dst, dst, 12);
    g_queue[idx].dst[12] = '\0';
    strncpy(g_queue[idx].topic, topic, 63);
    g_queue[idx].topic[63] = '\0';
    strncpy(g_queue[idx].payload, payload, 255);
    g_queue[idx].payload[255] = '\0';
    g_queue[idx].ts = millis();
    g_queue[idx].delivered = false;
    g_queueCount++;
}

}  // namespace

bool isActive() { return g_active; }

bool setEnabled(bool on) {
    bool prev = g_enabled;
    g_enabled = on;
    return prev;
}

bool detectFn() {
    return true;  // BLE is built into ESP32-S3
}

void setupFn() {
    g_active = true;

    BLEDevice::init("ForgeKey-Relay");

    // Create GATT server and service
    BLEServer* pServer = BLEDevice::createServer();
    BLEService* service = pServer->createService(FORGEKEY_RELAY_SERVICE_UUID);
    BLECharacteristic* charac = service->createCharacteristic(
        FORGEKEY_RELAY_CHAR_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY);
    service->start();

    // Start advertising the relay service
    BLEAdvertising* advertising = BLEDevice::getAdvertising();
    advertising->addServiceUUID(FORGEKEY_RELAY_SERVICE_UUID);
    advertising->setScanResponse(true);
    advertising->setMinPreferred(0x06);
    advertising->setMaxPreferred(0x0C);
    advertising->start();

    Serial.println("[CAP/ble_relay] initialized (queue_size=" + String(BLE_RELAY_QUEUE_SIZE) +
                   ", peer_timeout=" + String(BLE_RELAY_PEER_TIMEOUT_MS / 1000) + "s, msg_ttl=" +
                   String(BLE_RELAY_MSG_TTL_MS / 1000) + "s)");
}

void tickFn() {
    unsigned long now = millis();

    // Prune stale peers
    for (int i = g_peerCount - 1; i >= 0; i--) {
        if (now - g_peers[i].lastSeen > BLE_RELAY_PEER_TIMEOUT_MS) {
            g_peers[i] = g_peers[g_peerCount - 1];
            g_peerCount--;
        }
    }

    // Prune stale queue entries
    for (int i = g_queueCount - 1; i >= 0; i--) {
        int idx = (g_queueHead + i) % BLE_RELAY_QUEUE_SIZE;
        if (now - g_queue[idx].ts > BLE_RELAY_MSG_TTL_MS) {
            for (int j = i; j < g_queueCount - 1; j++) {
                int srcIdx = (g_queueHead + j + 1) % BLE_RELAY_QUEUE_SIZE;
                int dstIdx = (g_queueHead + j) % BLE_RELAY_QUEUE_SIZE;
                g_queue[dstIdx] = g_queue[srcIdx];
            }
            g_queueHead = (g_queueHead + g_queueCount) % BLE_RELAY_QUEUE_SIZE;
            g_queueCount--;
            i--;
        }
    }

    // Sync to MQTT if connected and has undelivered messages
    if (mqttClient.isConnected()) {
        syncQueueToMqtt();
    }

    // Push to peers if MQTT is down and relay is enabled
    if (!mqttClient.isConnected() && g_enabled) {
        for (int i = 0; i < g_peerCount; i++) {
            if (!g_peers[i].connected && !g_bleConnecting) {
                pushToPeer(i);
                break;
            }
        }
    }
}

}  // namespace BleRelay

REGISTER_CAPABILITY(ble_relay, "ble_relay",
                    BleRelay::detectFn,
                    BleRelay::setupFn,
                    BleRelay::tickFn,
                    "ble/peers")

#endif  // FORGEKEY_DISABLE_BLE_RELAY
