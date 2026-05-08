#ifndef FORGEKEY_LOCK_CAPABILITY_H
#define FORGEKEY_LOCK_CAPABILITY_H

#include <Arduino.h>

namespace LockCapability {

// Initialize the lock capability. Called during CapabilityRegistry::setupAll().
void begin();

// Tick the lock capability. Called every loop via CapabilityRegistry::tickAll().
void tick();

// Handle an incoming unlock command from MQTT.
// Called from main.cpp's command handler when a JWT unlock command arrives.
bool handleUnlockCommand(const char* token, long timestamp);

// Publish telemetry to the lock status topic.
// Returns true if the publish succeeded.
bool publishTelemetry();

// Get the current lock state as a string for web display.
const char* getStateName();

// Check if the lock capability is active.
bool isActive();

}  // namespace LockCapability

#endif
