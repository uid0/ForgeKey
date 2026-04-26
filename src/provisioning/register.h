#ifndef PROVISIONING_REGISTER_H
#define PROVISIONING_REGISTER_H

#include <Arduino.h>

struct DeviceCredentials {
    String deviceId;
    String mqttFirmwareTopic;  // OMS-controlled OTA dispatch topic
    String mqttPingsTopic;     // OMS-controlled occupancy publish topic
    String jwtToken;           // bearer for HTTPS uploads + MQTT auth
};

class Provisioning {
public:
    // Initialize NVS-backed storage and increment the persistent boot counter.
    void begin();

    // True if the device has not yet stored a successful registration response.
    bool isProvisioned() const;

    // Wipe stored credentials. Used when factory-resetting or when the
    // server tells us our identity is invalid (rare).
    void clear();

    // Returns the cached credentials. Only meaningful after isProvisioned().
    DeviceCredentials credentials() const;

    // Persistent boot count, incremented in begin(). Sent to OMS at register
    // time so back-end can correlate fleet stability metrics.
    uint32_t bootCount() const { return cachedBootCount; }

    // POST a multipart photo + JSON metadata to the OMS register endpoint.
    // On success persists credentials and returns true.
    //
    // jpegBuf/jpegLen own a freshly captured camera frame.
    // mac is the dash-less lowercase MAC address.
    // The "host" + "port" are typically OMS_HOST / OMS_PORT.
    bool registerDevice(const char* host, uint16_t port,
                        const String& mac,
                        const String& ipAddr,
                        const uint8_t* jpegBuf, size_t jpegLen);

private:
    DeviceCredentials creds;
    uint32_t cachedBootCount = 0;

    bool persist(const DeviceCredentials& c);
    void load();
};

extern Provisioning provisioning;

#endif
