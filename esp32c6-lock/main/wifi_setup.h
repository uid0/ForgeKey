/*
 * WiFi setup for ESP32-C6 lock device.
 * Handles STA/AP mode, captive portal, and NVS-stored WiFi credentials.
 *
 * This header declares the public interface only; the implementation lives in
 * wifi_setup.c.
 */

#ifndef FORGEKEY_LOCK_WIFI_SETUP_H
#define FORGEKEY_LOCK_WIFI_SETUP_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bring up NVS-backed WiFi storage and the esp_wifi/esp_netif stack.
 * Returns true once the stack is initialised.
 */
bool wifi_setup_init(void);

/*
 * Connect using stored or predefined credentials. If none work, start the
 * captive portal and wait up to timeout_secs for the user to supply some.
 * Returns true once associated.
 */
bool wifi_connect_with_portal(unsigned long timeout_secs);

/*
 * Erase stored WiFi credentials and reboot into the captive portal.
 */
void wifi_forget_and_restart(void);

#ifdef __cplusplus
}
#endif

#endif /* FORGEKEY_LOCK_WIFI_SETUP_H */
