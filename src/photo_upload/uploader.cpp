#include "uploader.h"

#include <WiFiClientSecure.h>

#include "security/oms_ca.h"

PhotoUploader photoUploader;

namespace {
constexpr const char* kBoundary = "----ForgekeyPhotoBoundary8a2";
}

void PhotoUploader::begin(const char* h, uint16_t p, const String& m) {
    host = h;
    port = p;
    mac = m;
}

bool PhotoUploader::shouldUpload(unsigned long nowMs, unsigned long intervalMs,
                                 unsigned long motionWindowMs) const {
    if (lastUpload != 0 && nowMs - lastUpload < intervalMs) return false;
    // First-ever upload always proceeds; otherwise gate on recent motion.
    if (lastUpload == 0) return true;
    if (lastMotionMs == 0) return false;
    return (nowMs - lastMotionMs) <= motionWindowMs;
}

PhotoUploader::Result PhotoUploader::uploadPhoto(const uint8_t* jpegBuf, size_t jpegLen,
                                                 unsigned long nowMs) {
    if (!jpegBuf || jpegLen == 0) return Result::TransportError;
    if (jwtToken.length() == 0) return Result::AuthExpired;

    String head;
    head.reserve(192);
    head += "--"; head += kBoundary; head += "\r\n";
    head += "Content-Disposition: form-data; name=\"photo\"; filename=\"area.jpg\"\r\n";
    head += "Content-Type: image/jpeg\r\n\r\n";

    String tail;
    tail.reserve(64);
    tail += "\r\n--"; tail += kBoundary; tail += "--\r\n";

    size_t total = head.length() + jpegLen + tail.length();

    WiFiClientSecure tls;
    tls.setCACert(kOmsCaPem);
    tls.setTimeout(15);
    if (!tls.connect(host.c_str(), port)) {
        Serial.println("photo: TLS connect failed");
        return Result::TransportError;
    }

    tls.printf("POST /api/forgekey/devices/%s/photo/ HTTP/1.1\r\n"
               "Host: %s\r\n"
               "Authorization: Bearer %s\r\n"
               "Content-Type: multipart/form-data; boundary=%s\r\n"
               "Content-Length: %u\r\n"
               "Connection: close\r\n\r\n",
               mac.c_str(), host.c_str(), jwtToken.c_str(),
               kBoundary, (unsigned)total);

    tls.print(head);
    const size_t kChunk = 1024;
    for (size_t off = 0; off < jpegLen; off += kChunk) {
        size_t n = (jpegLen - off < kChunk) ? (jpegLen - off) : kChunk;
        if (tls.write(jpegBuf + off, n) != n) {
            tls.stop();
            return Result::TransportError;
        }
    }
    tls.print(tail);

    String statusLine = tls.readStringUntil('\n');
    statusLine.trim();
    int sp1 = statusLine.indexOf(' ');
    int sp2 = (sp1 >= 0) ? statusLine.indexOf(' ', sp1 + 1) : -1;
    int code = (sp1 >= 0 && sp2 > sp1)
                   ? statusLine.substring(sp1 + 1, sp2).toInt()
                   : 0;
    tls.stop();

    if (code >= 200 && code < 300) {
        lastUpload = nowMs;
        Serial.printf("photo: uploaded %u bytes (HTTP %d)\n", (unsigned)jpegLen, code);
        return Result::Ok;
    }
    if (code == 401) {
        Serial.println("photo: 401, JWT rejected — re-register required");
        return Result::AuthExpired;
    }
    if (code >= 500) {
        Serial.printf("photo: server error %d\n", code);
        return Result::ServerError;
    }
    Serial.printf("photo: unexpected status %d\n", code);
    return Result::BadResponse;
}
