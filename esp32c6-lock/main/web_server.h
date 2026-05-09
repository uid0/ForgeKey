/*
 * Embedded web server for ESP32-C6 lock device.
 * esp_http_server serving status page and JSON API.
 */

#ifndef FORGEKEY_LOCK_WEB_SERVER_H
#define FORGEKEY_LOCK_WEB_SERVER_H

#include <stdbool.h>

/* Initialize and start the web server on port 80. */
void lock_web_server_init(void);

/* Stop the web server. */
void lock_web_server_deinit(void);

/* Tick the web server (handle pending connections). Call from main loop. */
void lock_web_server_tick(void);

/* Get the HTML status page content. */
const char* lock_web_server_get_html(void);

/* Get the JSON status API content. */
const char* lock_web_server_get_json_status(void);

#endif /* FORGEKEY_LOCK_WEB_SERVER_H */
