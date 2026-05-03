#ifndef FORGEKEY_CAPABILITIES_PEOPLE_COUNTER_H
#define FORGEKEY_CAPABILITIES_PEOPLE_COUNTER_H

#include <Arduino.h>

// Camera-based occupancy counter. Self-registers with the capability
// registry; this header exposes the small amount of surface that main()
// needs (the OMS first-boot photo capture) without forcing main to know
// about TFLite/camera internals.
namespace PeopleCounter {

// True after the capability has detected hardware and successfully run
// setup(). Use this from main() to decide whether to bother capturing a
// photo for OMS registration.
bool isActive();

// Capture a JPEG suitable for OMS first-boot registration. Caller must
// free(*outBuf). Returns false if the capability is not active or if the
// capture failed.
bool captureProvisioningPhoto(uint8_t** outBuf, size_t* outLen);

// Queue an operator-triggered one-shot photo capture + upload. The actual
// capture happens on the next tick once the inference task is idle and the
// network has quieted (same gating as the periodic uploader). When the
// upload completes (success or failure), the result is published on the
// device's MQTT status topic as:
//   {"cmd_ack":"capture","upload_status":"ok"|"failed","reason":"..."}
// Returns true if the request was queued; false if the capability is not
// active.
bool requestOneShotCapture();

}

#endif
