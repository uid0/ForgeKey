#ifndef WIFI_SETUP_CAPTIVE_H
#define WIFI_SETUP_CAPTIVE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>

namespace WifiSetup {

// AP password printed on the device sticker. Open-ish by design — the AP is
// only live during first-boot provisioning and the surface area is local.
#ifndef FORGEKEY_AP_PASSWORD
#define FORGEKEY_AP_PASSWORD "12345678"
#endif

constexpr size_t kMaxStoredProfiles = 5;

enum class DnsPolicy : uint8_t {
    Dhcp = 0,
    Static = 1,
    FallbackPublic = 2,
};

enum class EnterpriseType : uint8_t {
    None = 0,
    Peap = 1,
    Ttls = 2,
};

struct StaticIpConfig {
    bool enabled = false;
    IPAddress ip;
    IPAddress gateway;
    IPAddress subnet;
    IPAddress dns1;
    IPAddress dns2;
};

struct EnterpriseConfig {
    EnterpriseType type = EnterpriseType::None;
    String identity;
    String username;
    String password;
    String caCertPem;
};

struct WifiProfile {
    String ssid;
    String password;
    String label;
    int priority = 100;
    uint32_t lastSuccessEpoch = 0;
    StaticIpConfig staticIp;
    DnsPolicy dnsPolicy = DnsPolicy::Dhcp;
    IPAddress dns1;
    IPAddress dns2;
    EnterpriseConfig enterprise;
};

struct RuntimeHealth {
    int disconnectReason = 0;
    uint32_t reconnectCount = 0;
    int currentRssi = 0;
    int rssiMin = 0;
    int rssiMax = 0;
    int rssiAvg = 0;
    String bssid;
    int channel = 0;
    IPAddress ip;
    IPAddress subnet;
    IPAddress gateway;
    IPAddress dns1;
    IPAddress dns2;
    String ssid;
    String profileLabel;
    bool staticIp = false;
    const char* dnsPolicy = "dhcp";
};

using ReachabilityProbe = bool (*)(unsigned long timeoutMs);

// Build the AP SSID for this device: "ForgeKey-Setup-XXXX" using the last
// four hex chars of the MAC. Caller owns the returned String.
String apSsid();

// Block until the device is on WiFi, either by reusing stored creds or by
// running the captive portal AP until a user enters new ones. Returns true
// if connected, false if the portal timed out without creds.
bool connectOrPortal(unsigned long portalTimeoutSeconds = 0);

// Desired-state WiFi updates. Payload accepts either {"wifi":{...}} or the
// wifi object directly. Profiles are tested first; only profiles that can
// connect and pass the reachability probe are committed to NVS.
bool applyDesiredState(JsonVariantConst desired,
                       ReachabilityProbe reachabilityProbe,
                       String& resultMessage,
                       unsigned long connectTimeoutMs = 20000,
                       unsigned long reachabilityTimeoutMs = 5000);

bool loadProfiles(WifiProfile* profiles, size_t maxProfiles, size_t& count);
bool saveProfiles(const WifiProfile* profiles, size_t count);
RuntimeHealth health();
void appendHealthJson(String& payload);
void tickHealth();

// Forget stored WiFi credentials and reboot. Triggered by the
// forgekey/<mac>/config "forget_wifi" MQTT command or a physical reset.
void forgetAndRestart();

// Debug: dump full DNS state to Serial. Used for diagnosing DNS issues.
void debug_dns_state(const char* label);

}  // namespace WifiSetup

#endif
