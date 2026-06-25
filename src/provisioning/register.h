#ifndef PROVISIONING_REGISTER_H
#define PROVISIONING_REGISTER_H

#include <Arduino.h>

struct DeviceCredentials {
    String deviceId;
    String mqttFirmwareTopic;  // OMS-controlled OTA dispatch topic
    String mqttPingsTopic;     // OMS-controlled occupancy publish topic
    String clientCertificatePem;  // OMS-issued device certificate for mTLS
    String clientPrivateKeyPem;   // locally generated private key used for the CSR
    String commandPublicKeyPem;   // OMS public key used for signed command verification
    // Broker connection info returned by OMS at enrollment time. Empty
    // host means "no broker info on file yet — use compile-time fallback".
    String mqttBrokerHost;
    uint16_t mqttBrokerPort = 0;
    bool mqttBrokerUseTls = false;
};

class Provisioning {
public:
    // Initialize NVS-backed storage and increment the persistent boot counter.
    void begin();

    // True if the device has already stored a successful enrollment response.
    bool isProvisioned() const;

    // Wipe stored identity credentials. Used when re-enrolling or when the
    // server tells us our identity is invalid (rare). Leaves bootstrap token
    // and WiFi profiles intact.
    void clear();

    // Wipe local lifecycle credentials from NVS. When wipeBootstrapToken is
    // true, the short-lived bootstrap token override is also removed. When
    // wipeWifiProfiles is true, known ForgeKey WiFi profile namespaces are
    // erased too (the caller should also reset the WiFi driver credentials).
    bool wipeCredentials(bool wipeBootstrapToken, bool wipeWifiProfiles);

    // Returns the cached credentials. Only meaningful after isProvisioned().
    DeviceCredentials credentials() const;

    // Persistent boot count, incremented in begin(). Sent to OMS at enrollment
    // time so back-end can correlate fleet stability metrics.
    uint32_t bootCount() const { return cachedBootCount; }

    // Returns the bearer token currently used for the X-ForgeKey-Provisioning-Token
    // header. Falls back to the compile-time FORGEKEY_BOOTSTRAP_TOKEN if NVS
    // hasn't yet been overwritten by an operator-delivered short-lived token.
    String activeProvisioningToken() const;

    // Persist a new bootstrap token delivered over the MQTT config channel.
    // Returns true if the value was actually written (false on NVS error).
    bool setProvisioningToken(const String& token);

    // POST a multipart photo + JSON metadata to the OMS enrollment endpoint,
    // including a locally generated public key and CSR. On success persists the signed
    // certificate bundle and returns true.
    //
    // jpegBuf/jpegLen own a freshly captured camera frame.
    // mac is the dash-less lowercase MAC address.
    // The "host" + "port" are typically OMS_HOST / OMS_PORT.
    bool enrollDevice(const char* host, uint16_t port,
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
