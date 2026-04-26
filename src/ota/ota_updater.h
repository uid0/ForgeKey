#ifndef OTA_UPDATER_H
#define OTA_UPDATER_H

#include <Arduino.h>

class OtaUpdater {
public:
    struct Spec {
        String url;
        String sha256;     // 64 hex chars
        String version;
        bool mandatory = false;
    };

    void begin();

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

    static bool hexEq(const String& a, const String& b);
};

extern OtaUpdater otaUpdater;

#endif
