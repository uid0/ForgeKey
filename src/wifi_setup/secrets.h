#ifndef WIFI_SETUP_SECRETS_H
#define WIFI_SETUP_SECRETS_H

// Pull in dev-machine WiFi credentials from a gitignored sibling header.
// If secrets_local.h is absent the FORGEKEY_NETWORK_*_SSID/PASSWORD macros
// stay undefined and the WiFiMulti pre-portal path in captive.cpp compiles
// out — firmware then behaves identically to a clean checkout.
#if __has_include("secrets_local.h")
#  include "secrets_local.h"
#endif

#endif
