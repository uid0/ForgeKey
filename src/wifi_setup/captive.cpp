#include "captive.h"

#include <WiFi.h>
#include <WiFiManager.h>

namespace WifiSetup {

String apSsid() {
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    String last4 = mac.substring(mac.length() - 4);
    last4.toUpperCase();
    return String("ForgeKey-Setup-") + last4;
}

bool connectOrPortal(unsigned long portalTimeoutSeconds) {
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
