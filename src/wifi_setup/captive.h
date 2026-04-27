#ifndef WIFI_SETUP_CAPTIVE_H
#define WIFI_SETUP_CAPTIVE_H

#include <Arduino.h>

namespace WifiSetup {

// AP password printed on the device sticker. Open-ish by design — the AP is
// only live during first-boot provisioning and the surface area is local.
#ifndef FORGEKEY_AP_PASSWORD
#define FORGEKEY_AP_PASSWORD "12345678"
#endif

// Build the AP SSID for this device: "ForgeKey-Setup-XXXX" using the last
// four hex chars of the MAC. Caller owns the returned String.
String apSsid();

// Block until the device is on WiFi, either by reusing stored creds or by
// running the captive portal AP until a user enters new ones. Returns true
// if connected, false if the portal timed out without creds.
//
// On success the new SSID/PSK are persisted by WiFiManager (NVS-backed) so
// subsequent boots skip the portal.
bool connectOrPortal(unsigned long portalTimeoutSeconds = 0);

// Forget stored WiFi credentials and reboot. Triggered by the
// forgekey/<mac>/config "forget_wifi" MQTT command or a physical reset.
void forgetAndRestart();

}  // namespace WifiSetup

#endif
