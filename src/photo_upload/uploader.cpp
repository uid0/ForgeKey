#include "uploader.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "security/oms_ca.h"
#include "../capabilities/status_led/status_led.h"

PhotoUploader photoUploader;

namespace {
constexpr const char* kBoundary = "----ForgekeyPhotoBoundary8a2";
}

void PhotoUploader::begin(const char* h, uint16_t p, const String& m) {
    host = h;
    port = p;
    mac = m;
    hostIpResolved = false;
    nextUploadAllowedMs = 0;
    uploadBackoffMs = 15000;
    Serial.printf("[photo] init: host=%s port=%u mac=%s\n", host.c_str(), port, mac.c_str());
    if (WiFi.hostByName(host.c_str(), hostIp)) {
        hostIpResolved = true;
        Serial.printf("[photo] cached host IP: %s -> %s\n",
                      host.c_str(), hostIp.toString().c_str());
    } else {
        Serial.printf("[photo] host DNS not resolved at init: %s\n", host.c_str());
    }
}

bool PhotoUploader::shouldUpload(unsigned long nowMs, unsigned long intervalMs,
                                 unsigned long motionWindowMs,
                                 bool mqttConnected) const {
    if (!mqttConnected) return false;
    if (nowMs < nextUploadAllowedMs) return false;
    if (lastUpload != 0 && nowMs - lastUpload < intervalMs) return false;
    // First-ever upload always proceeds; otherwise gate on recent motion.
    if (lastUpload == 0) return true;
    if (lastMotionMs == 0) return false;
    return (nowMs - lastMotionMs) <= motionWindowMs;
}

PhotoUploader::Result PhotoUploader::uploadPhoto(const uint8_t* jpegBuf, size_t jpegLen,
                                                 unsigned long nowMs) {
    if (!jpegBuf || jpegLen == 0) {
        Serial.println("[photo] uploadPhoto FAILED: null/empty jpeg buffer");
        return Result::TransportError;
    }
    if (jwtToken.length() == 0) {
        Serial.println("[photo] uploadPhoto FAILED: no JWT token");
        return Result::AuthExpired;
    }

    String head;
    head.reserve(192);
    head += "--"; head += kBoundary; head += "\r\n";
    head += "Content-Disposition: form-data; name=\"photo\"; filename=\"area.jpg\"\r\n";
    head += "Content-Type: image/jpeg\r\n\r\n";

    String tail;
    tail.reserve(64);
    tail += "\r\n--"; tail += kBoundary; tail += "--\r\n";

    size_t total = head.length() + jpegLen + tail.length();
    Serial.printf("[photo] uploadPhoto: uploading %u bytes to %s:%u (total_payload=%u)\n",
                  (unsigned)jpegLen, host.c_str(), port, (unsigned)total);

    WiFiClientSecure tls;
    tls.setCACert(kOmsCaPem);
    tls.setTimeout(15);
    
    IPAddress serverIp;
    bool haveIp = hostIpResolved;
    if (!haveIp) {
        for (int retry = 0; retry < 3; retry++) {
            if (WiFi.hostByName(host.c_str(), hostIp)) {
                hostIpResolved = true;
                haveIp = true;
                Serial.printf("[photo] cached host IP: %s -> %s\n",
                              host.c_str(), hostIp.toString().c_str());
                break;
            }
            delay(500);
        }
    }

    Serial.printf("[photo] attempting TLS connect to %s:%u...\n", host.c_str(), port);
    // Use the hostname for TLS connect so certificate validation can verify
    // the server name. Connecting directly to the IP breaks hostname checks.
    Serial.printf("[photo] TLS verification host: %s\n", host.c_str());
    bool started = tls.connect(host.c_str(), port);
    if (!started) {
        Serial.printf("[photo] FAILED: TLS connect failed to %s:%u\n", host.c_str(), port);
        tls.stop();
        uploadBackoffMs = uploadBackoffMs < 120000 ? uploadBackoffMs * 2 : 120000;
        nextUploadAllowedMs = nowMs + uploadBackoffMs;
        Serial.printf("[photo] next upload attempt after %lu ms\n", uploadBackoffMs);
        return Result::TransportError;
    }
    Serial.println("[photo] TLS handshake successful, sending request...");

    tls.printf("POST /api/forgekey/devices/%s/photo/ HTTP/1.1\r\n"
               "Host: %s\r\n"
               "Authorization: Bearer %s\r\n"
               "Content-Type: multipart/form-data; boundary=%s\r\n"
               "Content-Length: %u\r\n"
               "Connection: close\r\n\r\n",
               mac.c_str(), host.c_str(), jwtToken.c_str(),
               kBoundary, (unsigned)total);

    tls.print(head);
    tls.flush();  // Send headers immediately before the long TFLite inference runs
    Serial.println("[photo] headers flushed, writing jpeg payload...");
    
    const size_t kChunk = 1024;
    for (size_t off = 0; off < jpegLen; off += kChunk) {
        size_t n = (jpegLen - off < kChunk) ? (jpegLen - off) : kChunk;
        if (tls.write(jpegBuf + off, n) != n) {
            Serial.printf("[photo] FAILED to write jpeg chunk (off=%u, n=%u)\n",
                          (unsigned)off, (unsigned)n);
            tls.stop();
            return Result::TransportError;
        }
    }
    tls.print(tail);
    tls.flush();  // Ensure tail is sent before reading response
    Serial.println("[photo] payload complete, reading response...");

    String statusLine = tls.readStringUntil('\n');
    statusLine.trim();
    Serial.printf("[photo] response: %s\n", statusLine.c_str());
    int sp1 = statusLine.indexOf(' ');
    int sp2 = (sp1 >= 0) ? statusLine.indexOf(' ', sp1 + 1) : -1;
    int code = (sp1 >= 0 && sp2 > sp1)
                   ? statusLine.substring(sp1 + 1, sp2).toInt()
                   : 0;
    tls.stop();

    if (code >= 200 && code < 300) {
        lastUpload = nowMs;
        uploadBackoffMs = 15000;
        nextUploadAllowedMs = 0;
        Serial.printf("[photo] SUCCESS: uploaded %u bytes (HTTP %d)\n", (unsigned)jpegLen, code);
        // Flash LED to indicate HTTP POST completed
        StatusLed::triggerMessageFlash();
        return Result::Ok;
    }
    if (code == 401) {
        Serial.println("[photo] FAILED: 401 Unauthorized — JWT rejected, re-register required");
        return Result::AuthExpired;
    }
    if (code >= 500) {
        uploadBackoffMs = uploadBackoffMs < 120000 ? uploadBackoffMs * 2 : 120000;
        nextUploadAllowedMs = nowMs + uploadBackoffMs;
        Serial.printf("[photo] FAILED: server error HTTP %d, backoff %lu ms\n", code, uploadBackoffMs);
        return Result::ServerError;
    }
    uploadBackoffMs = uploadBackoffMs < 120000 ? uploadBackoffMs * 2 : 120000;
    nextUploadAllowedMs = nowMs + uploadBackoffMs;
    Serial.printf("[photo] FAILED: unexpected HTTP status %d, backoff %lu ms\n", code, uploadBackoffMs);
    return Result::BadResponse;
}
