#ifndef PHOTO_UPLOAD_UPLOADER_H
#define PHOTO_UPLOAD_UPLOADER_H

#include <Arduino.h>

class PhotoUploader {
public:
    enum class Result {
        Ok,
        Skipped,            // gated by motion window
        TransportError,     // TLS / socket failure
        AuthExpired,        // 401 — caller must re-register and retry
        ServerError,        // 5xx
        BadResponse         // non-2xx, non-401, non-5xx
    };

    void begin(const char* host, uint16_t port,
               const String& mac);
    void setJwt(const String& jwt) { jwtToken = jwt; }

    // Mark that motion was observed at `nowMs`. The uploader will skip
    // the next periodic capture if no motion has happened recently.
    void markMotion(unsigned long nowMs) { lastMotionMs = nowMs; }

    // True if enough time has elapsed since the last upload AND motion
    // has been observed within the gating window. Uploads are also
    // suppressed while MQTT is disconnected.
    bool shouldUpload(unsigned long nowMs, unsigned long intervalMs,
                      unsigned long motionWindowMs,
                      bool mqttConnected) const;

    // Upload a JPEG buffer. Returns the parsed status.
    Result uploadPhoto(const uint8_t* jpegBuf, size_t jpegLen,
                       unsigned long nowMs);

    unsigned long lastUploadMs() const { return lastUpload; }

private:
    String host;
    IPAddress hostIp;
    bool hostIpResolved = false;
    uint16_t port = 443;
    String mac;
    String jwtToken;
    unsigned long lastUpload = 0;
    unsigned long lastMotionMs = 0;
    unsigned long nextUploadAllowedMs = 0;
    unsigned long uploadBackoffMs = 15000;
};

extern PhotoUploader photoUploader;

#endif
