#ifndef CREDENTIAL_ROTATION_H
#define CREDENTIAL_ROTATION_H

#include <Arduino.h>
#include <stdint.h>

// Handles config-channel MQTT messages on forgekey/<mac>/config that
// rotate the shared provisioning bearer token. The expected payload is
//   {"provisioning_token":"<new>","valid_after":"<rfc3339>"}
// We store the new token in NVS immediately so the next re-registration
// uses it. valid_after is parsed and logged, but coordination of the
// cutover is the back-end's responsibility (it controls when the old
// token stops being accepted by /api/forgekey/devices/register/).
namespace credential_rotation {

// MQTT message handler. Safe to register as a PubSubClient callback —
// it does its own JSON parse and NVS write.
void onConfigMessage(const char* topic, const uint8_t* payload, unsigned int length);

// Build the per-device topic from the bare MAC string (no separators).
String topicFor(const String& mac);

}  // namespace credential_rotation

#endif
