#include "register.h"
#include "device_config.h"

#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

Provisioning provisioning;

namespace {
constexpr const char* kNvsNamespace = "forgekey";
constexpr const char* kKeyDeviceId = "dev_id";
constexpr const char* kKeyFwTopic  = "fw_topic";
constexpr const char* kKeyPingTopic = "p_topic";
constexpr const char* kKeyJwt      = "jwt";
constexpr const char* kKeyBoots    = "boots";

// Multipart boundary used for the multipart/form-data registration POST.
constexpr const char* kBoundary = "----ForgekeyBoundary7d3f2c1a";
}  // namespace

void Provisioning::begin() {
    Preferences p;
    p.begin(kNvsNamespace, /*readOnly=*/false);
    cachedBootCount = p.getUInt(kKeyBoots, 0) + 1;
    p.putUInt(kKeyBoots, cachedBootCount);
    p.end();

    load();
}

void Provisioning::load() {
    Preferences p;
    p.begin(kNvsNamespace, /*readOnly=*/true);
    creds.deviceId          = p.getString(kKeyDeviceId, "");
    creds.mqttFirmwareTopic = p.getString(kKeyFwTopic, "");
    creds.mqttPingsTopic    = p.getString(kKeyPingTopic, "");
    creds.jwtToken          = p.getString(kKeyJwt, "");
    p.end();
}

bool Provisioning::isProvisioned() const {
    return creds.deviceId.length() > 0 && creds.jwtToken.length() > 0;
}

void Provisioning::clear() {
    Preferences p;
    p.begin(kNvsNamespace, /*readOnly=*/false);
    p.remove(kKeyDeviceId);
    p.remove(kKeyFwTopic);
    p.remove(kKeyPingTopic);
    p.remove(kKeyJwt);
    p.end();
    creds = DeviceCredentials{};
}

DeviceCredentials Provisioning::credentials() const { return creds; }

bool Provisioning::persist(const DeviceCredentials& c) {
    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/false)) return false;
    p.putString(kKeyDeviceId, c.deviceId);
    p.putString(kKeyFwTopic, c.mqttFirmwareTopic);
    p.putString(kKeyPingTopic, c.mqttPingsTopic);
    p.putString(kKeyJwt, c.jwtToken);
    p.end();
    creds = c;
    return true;
}

bool Provisioning::registerDevice(const char* host, uint16_t port,
                                  const String& mac,
                                  const String& ipAddr,
                                  const uint8_t* jpegBuf, size_t jpegLen) {
    if (!jpegBuf || jpegLen == 0) {
        Serial.println("register: missing photo");
        return false;
    }

    // Build the JSON metadata part once so we can compute Content-Length.
    StaticJsonDocument<384> meta;
    meta["mac_address"]      = mac;
    meta["firmware_version"] = FORGEKEY_FIRMWARE_VERSION;
    meta["sensor_kind"]      = FORGEKEY_SENSOR_KIND;
    meta["boot_count"]       = cachedBootCount;
    meta["free_heap"]        = ESP.getFreeHeap();
    meta["ip"]               = ipAddr;
    String metaJson;
    serializeJson(meta, metaJson);

    String head;
    head.reserve(512);
    head += "--"; head += kBoundary; head += "\r\n";
    head += "Content-Disposition: form-data; name=\"metadata\"\r\n";
    head += "Content-Type: application/json\r\n\r\n";
    head += metaJson;
    head += "\r\n--"; head += kBoundary; head += "\r\n";
    head += "Content-Disposition: form-data; name=\"photo\"; filename=\"boot.jpg\"\r\n";
    head += "Content-Type: image/jpeg\r\n\r\n";

    String tail;
    tail.reserve(64);
    tail += "\r\n--"; tail += kBoundary; tail += "--\r\n";

    size_t totalLen = head.length() + jpegLen + tail.length();

    WiFiClientSecure tls;
    tls.setInsecure();  // v1: rely on TLS for transit only; CA pinning lands later
    tls.setTimeout(15);
    if (!tls.connect(host, port)) {
        Serial.printf("register: TLS connect to %s:%u failed\n", host, port);
        return false;
    }

    tls.printf("POST /api/forgekey/devices/register/ HTTP/1.1\r\n"
               "Host: %s\r\n"
               "X-ForgeKey-Provisioning-Token: %s\r\n"
               "Content-Type: multipart/form-data; boundary=%s\r\n"
               "Content-Length: %u\r\n"
               "Connection: close\r\n\r\n",
               host, FORGEKEY_PROVISIONING_TOKEN, kBoundary,
               (unsigned)totalLen);
    tls.print(head);
    // Stream the JPEG in chunks so we don't double-allocate the buffer.
    const size_t kChunk = 1024;
    for (size_t off = 0; off < jpegLen; off += kChunk) {
        size_t n = (jpegLen - off < kChunk) ? (jpegLen - off) : kChunk;
        if (tls.write(jpegBuf + off, n) != n) {
            Serial.println("register: TLS write failed mid-photo");
            tls.stop();
            return false;
        }
    }
    tls.print(tail);

    // Read status line.
    String statusLine = tls.readStringUntil('\n');
    statusLine.trim();
    int sp1 = statusLine.indexOf(' ');
    int sp2 = (sp1 >= 0) ? statusLine.indexOf(' ', sp1 + 1) : -1;
    int code = (sp1 >= 0 && sp2 > sp1)
                   ? statusLine.substring(sp1 + 1, sp2).toInt()
                   : 0;
    if (code < 200 || code >= 300) {
        Serial.printf("register: status %d (%s)\n", code, statusLine.c_str());
        tls.stop();
        return false;
    }

    // Skip headers up to blank line.
    while (tls.connected() || tls.available()) {
        String line = tls.readStringUntil('\n');
        if (line.length() <= 1) break;  // "\r" only
    }

    // Read the body (server is expected to keep it small — < 1KB).
    String body;
    body.reserve(512);
    while (tls.available() || tls.connected()) {
        while (tls.available()) body += (char)tls.read();
        if (!tls.available()) delay(10);
        if (body.length() > 4096) break;  // hard cap, shouldn't happen
    }
    tls.stop();

    StaticJsonDocument<512> resp;
    DeserializationError err = deserializeJson(resp, body);
    if (err) {
        Serial.printf("register: JSON parse error: %s\n", err.c_str());
        return false;
    }

    DeviceCredentials c;
    c.deviceId          = (const char*)(resp["device_id"]          | "");
    c.mqttFirmwareTopic = (const char*)(resp["mqtt_topic_for_firmware"] | "");
    c.mqttPingsTopic    = (const char*)(resp["mqtt_topic_for_pings"]    | "");
    c.jwtToken          = (const char*)(resp["jwt_token"]               | "");
    if (c.deviceId.length() == 0 || c.jwtToken.length() == 0) {
        Serial.println("register: response missing device_id or jwt_token");
        return false;
    }

    if (!persist(c)) {
        Serial.println("register: NVS persist failed");
        return false;
    }
    Serial.printf("register: provisioned as %s\n", c.deviceId.c_str());
    return true;
}
