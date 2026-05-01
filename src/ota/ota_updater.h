#ifndef OTA_UPDATER_H
#define OTA_UPDATER_H

#include <Arduino.h>
#include <functional>

class OtaUpdater {
public:
    struct Spec {
        String url;
        String sha256;       // 64 hex chars
        String signature;    // base64-encoded ECDSA(P-256) DER over the raw image
        String version;
        bool mandatory = false;
    };

    // Status callback fired at each OTA lifecycle transition. `progress` is
    // 0..100 during downloading; -1 when not applicable. `error` is empty on
    // success-path states, populated on failure states. The callback is best-
    // effort: a slow handler must not stall the download loop, so callers
    // should keep work cheap (e.g. publish one MQTT message and return).
    using StatusCallback = std::function<void(const char* state,
                                              const char* version,
                                              int progress,
                                              const char* error)>;

    void begin();
    void setStatusCallback(StatusCallback cb) { statusCb = cb; }

    // Parse a JSON payload (the MQTT firmware-dispatch message) into a Spec.
    // Returns false if any required field is missing/malformed.
    bool parse(const uint8_t* payload, unsigned int length, Spec& out);

    // Download from spec.url, stream-write to the OTA partition, verify
    // SHA-256, mark the new partition pending and reboot. Does not return
    // on success. Returns false (with logs) on any verification failure.
    bool apply(const Spec& spec);

    // Call after the first successful occupancy publish post-reboot. If the
    // bootloader booted into a pending partition, marks it valid so the
    // next reboot won't roll back. Idempotent / cheap.
    void markStableIfPending();

    // True if we're currently inside apply(); the main loop should defer
    // non-essential work like photo uploads while this is set.
    bool inProgress() const { return updating; }

private:
    bool updating = false;
    bool stableMarked = false;
    StatusCallback statusCb;

    void notify(const char* state, const char* version, int progress, const char* error);
    static bool hexEq(const String& a, const String& b);
};

extern OtaUpdater otaUpdater;

#endif
