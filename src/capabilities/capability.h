#ifndef FORGEKEY_CAPABILITIES_CAPABILITY_H
#define FORGEKEY_CAPABILITIES_CAPABILITY_H

#include <Arduino.h>

// Self-describing piece of hardware/feature support.
//
// Each capability lives in its own translation unit, defines its three
// lifecycle hooks, and registers a Capability via REGISTER_CAPABILITY at
// static-init time. The registry collects them into a singly-linked list and
// the firmware iterates that list at boot (detect -> setup) and per loop
// (tick).
//
// detect():
//   Probe the underlying hardware. Cheap. Must not assume WiFi/MQTT/NVS are
//   ready - runs early in setup() to determine which capabilities are
//   physically present on this board.
//
// setup():
//   Called once if detect() returned true. Initialise anything needed
//   (allocate buffers, configure pins, start drivers). May log freely.
//
// tick():
//   Called every main-loop iteration. Capabilities own their own cadence
//   internally; this is just the heartbeat.
//
// `topic_suffix` is the leaf segment under
// forgekey/<mac>/<id>/<topic_suffix> that this capability publishes to. It
// is informational; the actual publish path is whatever the capability
// implements.
struct Capability {
    const char* id;                  // stable id, e.g. "people_counter"
    bool (*detect)();                // probe hardware
    void (*setup)();                 // one-time init if detected
    void (*tick)();                  // main-loop hook
    const char* topic_suffix;        // e.g. "occupancy", or nullptr
    bool active;                     // set by registry after detect() == true
    bool first_tick_logged;          // registry-managed
    Capability* next;                // intrusive list pointer (registry-owned)
};

// Push `cap` onto the registry. Safe to call from a constructor; runs
// before main()/setup().
void registerCapability(Capability* cap);

// Define a Capability instance and arrange for it to be registered before
// setup() runs. Place inside a translation unit (typically the .cpp for the
// capability). IDENT must be a valid C identifier and unique within the
// firmware - linker collisions catch duplicates.
#define REGISTER_CAPABILITY(IDENT, ID_STR, DETECT_FN, SETUP_FN, TICK_FN, TOPIC_SUFFIX) \
    static Capability IDENT##_capability = { \
        (ID_STR), (DETECT_FN), (SETUP_FN), (TICK_FN), \
        (TOPIC_SUFFIX), false, false, nullptr \
    }; \
    namespace { \
    struct IDENT##_register_t { \
        IDENT##_register_t() { ::registerCapability(&IDENT##_capability); } \
    }; \
    static IDENT##_register_t IDENT##_register_instance; \
    }

#endif
