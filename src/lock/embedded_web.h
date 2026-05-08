#ifndef FORGEKEY_LOCK_EMBEDDED_WEB_H
#define FORGEKEY_LOCK_EMBEDDED_WEB_H

#include <Arduino.h>

namespace LockWeb {

// Initialize and start the embedded web server.
// Serves a status page at http://<device-ip>/
// Shows lock state and a link to OpenMakerSuite for generating unlock codes.
void begin();

// Tick the web server (handle client connections). Call from main loop.
void tick();

// Stop the web server.
void end();

// Get the current HTML content for the status page.
String getStatusPageHtml();

}  // namespace LockWeb

#endif
