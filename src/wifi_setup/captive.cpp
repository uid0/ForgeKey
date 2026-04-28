#include "captive.h"

#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiMulti.h>

#include "secrets.h"

namespace WifiSetup {

namespace {

#if defined(FORGEKEY_NETWORK_1_SSID) || defined(FORGEKEY_NETWORK_2_SSID)
#define FORGEKEY_HAS_PREDEFINED_NETWORKS 1
#else
#define FORGEKEY_HAS_PREDEFINED_NETWORKS 0
#endif

#if FORGEKEY_HAS_PREDEFINED_NETWORKS
// Per-attempt timeout for WiFiMulti.run(). Long enough for a slow AP scan
// + DHCP, short enough that a missing dev network doesn't noticeably
// delay portal fallback.
constexpr unsigned long kPredefinedTimeoutMs = 15000;

bool tryPredefinedNetworks() {
    WiFiMulti multi;
#ifdef FORGEKEY_NETWORK_1_SSID
    multi.addAP(FORGEKEY_NETWORK_1_SSID, FORGEKEY_NETWORK_1_PASSWORD);
#endif
#ifdef FORGEKEY_NETWORK_2_SSID
    multi.addAP(FORGEKEY_NETWORK_2_SSID, FORGEKEY_NETWORK_2_PASSWORD);
#endif
    Serial.println("wifi: trying predefined networks...");
    WiFi.mode(WIFI_STA);
    if (multi.run(kPredefinedTimeoutMs) == WL_CONNECTED) {
        Serial.printf("wifi: connected to '%s' via predefined list\n",
                      WiFi.SSID().c_str());
        return true;
    }
    Serial.println("wifi: predefined networks unavailable, falling back");
    return false;
}
#endif  // FORGEKEY_HAS_PREDEFINED_NETWORKS

}  // namespace

String apSsid() {
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    String last4 = mac.substring(mac.length() - 4);
    last4.toUpperCase();
    return String("ForgeKey-Setup-") + last4;
}

bool connectOrPortal(unsigned long portalTimeoutSeconds) {
#if FORGEKEY_HAS_PREDEFINED_NETWORKS
    if (tryPredefinedNetworks()) {
        return true;
    }
#endif

    WiFiManager wm;
    wm.setDebugOutput(false);
    wm.setConfigPortalBlocking(true);
    if (portalTimeoutSeconds > 0) {
        wm.setConfigPortalTimeout(portalTimeoutSeconds);
    }

    String ssid = apSsid();
    Serial.printf("wifi: AP SSID '%s' (pw '%s')\n",
                  ssid.c_str(), FORGEKEY_AP_PASSWORD);

    // autoConnect tries the saved creds first; if missing or failing it
    // raises the AP + DNS captive portal and blocks until the user submits
    // creds (or the optional timeout fires).
    return wm.autoConnect(ssid.c_str(), FORGEKEY_AP_PASSWORD);
}

void forgetAndRestart() {
    Serial.println("wifi: forgetting credentials, restarting into portal");
    WiFiManager wm;
    wm.resetSettings();
    delay(200);
    ESP.restart();
}

}  // namespace WifiSetup
