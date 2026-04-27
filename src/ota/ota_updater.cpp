#include "ota_updater.h"

#include <ArduinoJson.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <WiFiClient.h>
#include <mbedtls/sha256.h>
#include <esp_ota_ops.h>

#include "security/oms_ca.h"
#include "security/firmware_verify.h"

OtaUpdater otaUpdater;

void OtaUpdater::begin() {
    // Nothing to set up at boot. markStableIfPending() handles validation
    // once the rest of the system has proven itself.
}

bool OtaUpdater::parse(const uint8_t* payload, unsigned int length, Spec& out) {
    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
        Serial.printf("ota: parse error: %s\n", err.c_str());
        return false;
    }
    out.url       = (const char*)(doc["url"]       | "");
    out.sha256    = (const char*)(doc["sha256"]    | "");
    out.signature = (const char*)(doc["signature"] | "");
    out.version   = (const char*)(doc["version"]   | "");
    out.mandatory = doc["mandatory"] | false;
    if (out.url.length() == 0 || out.sha256.length() != 64) {
        Serial.println("ota: missing url or invalid sha256");
        return false;
    }
    if (out.signature.length() == 0) {
        Serial.println("ota: dispatch missing signature — refusing");
        return false;
    }
    out.sha256.toLowerCase();
    return true;
}

bool OtaUpdater::hexEq(const String& a, const String& b) {
    if (a.length() != b.length()) return false;
    for (size_t i = 0; i < a.length(); i++) {
        char ca = a[i]; if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        char cb = b[i]; if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

namespace {

// Parse a URL of the form https://host[:port]/path or http://host[:port]/path.
bool splitUrl(const String& url, bool& tls, String& host, uint16_t& port, String& path) {
    int schemeEnd = url.indexOf("://");
    if (schemeEnd < 0) return false;
    String scheme = url.substring(0, schemeEnd);
    scheme.toLowerCase();
    if (scheme == "https") { tls = true;  port = 443; }
    else if (scheme == "http") { tls = false; port = 80; }
    else return false;

    int hostStart = schemeEnd + 3;
    int pathStart = url.indexOf('/', hostStart);
    String hostPort = (pathStart < 0) ? url.substring(hostStart)
                                      : url.substring(hostStart, pathStart);
    int colon = hostPort.indexOf(':');
    if (colon >= 0) {
        host = hostPort.substring(0, colon);
        port = (uint16_t)hostPort.substring(colon + 1).toInt();
    } else {
        host = hostPort;
    }
    path = (pathStart < 0) ? "/" : url.substring(pathStart);
    return host.length() > 0;
}

const char* kHexDigits = "0123456789abcdef";

void toHex(const uint8_t* in, size_t n, char* outHex) {
    for (size_t i = 0; i < n; i++) {
        outHex[i * 2]     = kHexDigits[(in[i] >> 4) & 0xF];
        outHex[i * 2 + 1] = kHexDigits[in[i] & 0xF];
    }
    outHex[n * 2] = '\0';
}

}  // namespace

bool OtaUpdater::apply(const Spec& spec) {
    bool tls = false;
    String host, path;
    uint16_t port = 0;
    if (!splitUrl(spec.url, tls, host, port, path)) {
        Serial.println("ota: malformed URL");
        return false;
    }

    updating = true;

    WiFiClient* client = nullptr;
    WiFiClientSecure secureClient;
    WiFiClient plainClient;
    if (tls) {
        secureClient.setCACert(kOmsCaPem);
        secureClient.setTimeout(20);
        client = &secureClient;
    } else {
        client = &plainClient;
    }

    if (!client->connect(host.c_str(), port)) {
        Serial.printf("ota: connect %s:%u failed\n", host.c_str(), port);
        updating = false;
        return false;
    }

    // Issue the request. Many origins require Host + close + UA.
    client->printf("GET %s HTTP/1.1\r\n"
                   "Host: %s\r\n"
                   "User-Agent: ForgeKey-OTA/1\r\n"
                   "Accept: */*\r\n"
                   "Connection: close\r\n\r\n",
                   path.c_str(), host.c_str());

    // Status line.
    String statusLine = client->readStringUntil('\n');
    statusLine.trim();
    int sp1 = statusLine.indexOf(' ');
    int sp2 = (sp1 >= 0) ? statusLine.indexOf(' ', sp1 + 1) : -1;
    int code = (sp1 >= 0 && sp2 > sp1)
                   ? statusLine.substring(sp1 + 1, sp2).toInt()
                   : 0;
    if (code != 200) {
        Serial.printf("ota: HTTP %d (%s)\n", code, statusLine.c_str());
        client->stop();
        updating = false;
        return false;
    }

    // Headers.
    size_t contentLength = 0;
    while (true) {
        String line = client->readStringUntil('\n');
        line.trim();
        if (line.length() == 0) break;
        if (line.startsWith("Content-Length:") || line.startsWith("content-length:")) {
            contentLength = (size_t)line.substring(line.indexOf(':') + 1).toInt();
        }
    }

    if (contentLength == 0) {
        Serial.println("ota: server did not advertise Content-Length");
        client->stop();
        updating = false;
        return false;
    }
    Serial.printf("ota: downloading %u bytes for %s\n",
                  (unsigned)contentLength, spec.version.c_str());

    if (!Update.begin(contentLength)) {
        Serial.printf("ota: Update.begin failed: %s\n", Update.errorString());
        client->stop();
        updating = false;
        return false;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, /*is224=*/0);

    uint8_t buf[1024];
    size_t received = 0;
    unsigned long lastDataMs = millis();
    while (received < contentLength) {
        size_t avail = client->available();
        if (avail == 0) {
            if (!client->connected()) {
                Serial.println("ota: connection closed mid-download");
                Update.abort();
                mbedtls_sha256_free(&sha);
                client->stop();
                updating = false;
                return false;
            }
            if (millis() - lastDataMs > 20000) {
                Serial.println("ota: read timeout");
                Update.abort();
                mbedtls_sha256_free(&sha);
                client->stop();
                updating = false;
                return false;
            }
            delay(5);
            continue;
        }
        size_t want = sizeof(buf);
        if (want > avail) want = avail;
        if (received + want > contentLength) want = contentLength - received;
        int n = client->read(buf, want);
        if (n <= 0) { delay(2); continue; }
        if ((size_t)Update.write(buf, n) != (size_t)n) {
            Serial.printf("ota: write failed: %s\n", Update.errorString());
            Update.abort();
            mbedtls_sha256_free(&sha);
            client->stop();
            updating = false;
            return false;
        }
        mbedtls_sha256_update(&sha, buf, n);
        received += n;
        lastDataMs = millis();
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    client->stop();

    char actualHex[65];
    toHex(digest, 32, actualHex);
    if (!hexEq(String(actualHex), spec.sha256)) {
        Serial.printf("ota: checksum mismatch — got %s expected %s\n",
                      actualHex, spec.sha256.c_str());
        Update.abort();
        updating = false;
        return false;
    }

    // ECDSA signature must verify before we hand the partition to the bootloader.
    // A mismatch here means either the binary was tampered with in transit (and
    // collided on SHA-256, which is implausible) or the dispatcher used a key the
    // device doesn't trust — either way we refuse the swap.
    {
        uint8_t sigBuf[128];  // ECDSA(P-256) DER is ≤ 72 bytes; 128 is comfortable headroom
        size_t sigLen = sizeof(sigBuf);
        if (!firmware_verify::decodeBase64(spec.signature, sigBuf, &sigLen)) {
            Serial.println("ota: signature is not valid base64 — abort");
            Update.abort();
            updating = false;
            return false;
        }
        if (!firmware_verify::verifySignature(digest, 32, sigBuf, sigLen)) {
            Serial.println("ota: signature verification failed — abort");
            Update.abort();
            updating = false;
            return false;
        }
        Serial.println("ota: signature verified");
    }

    if (!Update.end(/*evenIfRemaining=*/true)) {
        Serial.printf("ota: Update.end failed: %s\n", Update.errorString());
        updating = false;
        return false;
    }

    Serial.printf("ota: %s installed, rebooting\n", spec.version.c_str());
    delay(250);
    ESP.restart();
    // unreachable
    return true;
}

void OtaUpdater::markStableIfPending() {
    if (stableMarked) return;
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) return;
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            Serial.println("ota: marked running partition as valid");
        } else {
            Serial.printf("ota: mark valid failed: 0x%x\n", err);
        }
    }
    stableMarked = true;
}
