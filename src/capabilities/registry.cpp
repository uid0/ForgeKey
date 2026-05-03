#include "registry.h"
#include <Arduino.h>

namespace {
Capability* g_head = nullptr;
Capability* g_tail = nullptr;
int g_count = 0;
}

void registerCapability(Capability* cap) {
    if (!cap) return;
    cap->next = nullptr;
    if (!g_head) {
        g_head = cap;
        g_tail = cap;
    } else {
        g_tail->next = cap;
        g_tail = cap;
    }
    ++g_count;
}

namespace CapabilityRegistry {

Capability* head() { return g_head; }

int count() { return g_count; }

int activeCount() {
    int n = 0;
    for (Capability* c = g_head; c; c = c->next) {
        if (c->active) ++n;
    }
    return n;
}

void detectAll() {
    Serial.printf("[CAP] detectAll: %d capability(ies) registered\n", g_count);
    for (Capability* c = g_head; c; c = c->next) {
        if (!c->detect) {
            Serial.printf("[CAP/%s] detect() = SKIP (no detect fn)\n", c->id);
            c->active = false;
            continue;
        }
        bool ok = c->detect();
        c->active = ok;
        Serial.printf("[CAP/%s] detect() = %s\n", c->id, ok ? "true" : "false");
    }
    Serial.printf("[CAP] detectAll: %d active\n", activeCount());
}

void setupAll() {
    for (Capability* c = g_head; c; c = c->next) {
        if (!c->active) continue;
        Serial.printf("[CAP/%s] setup() begin\n", c->id);
        if (c->setup) {
            c->setup();
        }
        Serial.printf("[CAP/%s] setup() complete\n", c->id);
    }
}

void tickAll() {
    for (Capability* c = g_head; c; c = c->next) {
        if (!c->active || !c->tick) continue;
        if (!c->first_tick_logged) {
            Serial.printf("[CAP/%s] first tick()\n", c->id);
            c->first_tick_logged = true;
        }
        c->tick();
    }
}

String announcementJson(const char* firmwareVersion) {
    String out;
    out.reserve(192);
    out += "{\"capabilities\":[";
    bool first = true;
    for (Capability* c = g_head; c; c = c->next) {
        if (!c->active) continue;
        if (!first) out += ",";
        out += "\"";
        out += c->id;
        out += "\"";
        first = false;
    }
    out += "],\"firmware_version\":\"";
    out += firmwareVersion ? firmwareVersion : "";
    out += "\"}";
    return out;
}

}
