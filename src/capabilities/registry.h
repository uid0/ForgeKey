#ifndef FORGEKEY_CAPABILITIES_REGISTRY_H
#define FORGEKEY_CAPABILITIES_REGISTRY_H

#include "capability.h"

namespace CapabilityRegistry {

// First node in the registration list. Order is the static-init order of
// the .cpp files containing REGISTER_CAPABILITY. Iterate via cap->next.
Capability* head();

// Total registered count (active or not).
int count();

// Currently-active count.
int activeCount();

// Run detect() on every registered capability. For each one whose detect()
// returns true, mark it active. Logs the boolean per capability (AC-5).
void detectAll();

// Run setup() on every active capability. Logs entry/exit per capability
// (AC-5).
void setupAll();

// Run tick() on every active capability. Logs the very first tick of each
// capability; subsequent ticks are silent.
void tickAll();

// Build the JSON announcement payload for forgekey/<mac>/capabilities. Looks
// like: {"capabilities":["people_counter","status_led"],
//        "firmware_version":"<v>"}
String announcementJson(const char* firmwareVersion);

}

#endif
