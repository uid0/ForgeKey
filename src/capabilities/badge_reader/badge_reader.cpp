#include "badge_reader.h"

// The capability is "built" when a reader driver flag is set. Either driver
// (PN532 reference or mock) enables the full publish pipeline; with neither, the
// translation unit collapses to nothing and nothing registers.
#if defined(FORGEKEY_BADGE_READER_PN532) || defined(FORGEKEY_BADGE_READER_MOCK)

#include <ArduinoJson.h>
#include <esp_random.h>

#include "capabilities/capability.h"
#include "boards/board_manifest.h"
#include "mqtt/mqtt_client.h"
#include "time/time_sync.h"

#ifndef FORGEKEY_BADGE_READER_POLL_INTERVAL_MS
#define FORGEKEY_BADGE_READER_POLL_INTERVAL_MS 50UL
#endif

// One tap == one event. Repeated reads of the SAME UID within this window (a
// card held on the reader returns its UID on every poll) collapse to a single
// access-request event. Keep this value in sync with the host contract test
// tests/host/test_protocol_contracts.py::BadgeDebouncer.
#ifndef FORGEKEY_BADGE_READER_DEBOUNCE_MS
#define FORGEKEY_BADGE_READER_DEBOUNCE_MS 2000UL
#endif

namespace BadgeReader {

bool detectFn();
void setupFn();
void tickFn();

namespace {

Reader* g_reader = nullptr;
bool g_ready = false;
unsigned long g_lastPollMs = 0;

// Debounce state: the last UID we published an event for, and when.
CardUid g_lastUid = {};
bool g_haveLast = false;
unsigned long g_lastPublishMs = 0;

bool sameUid(const CardUid& a, const CardUid& b) {
    return a.length == b.length && memcmp(a.bytes, b.bytes, a.length) == 0;
}

// Debounce decision. A different UID always publishes; the same UID only
// re-publishes once the debounce window has elapsed since the last event.
bool shouldPublish(const CardUid& uid, unsigned long now) {
    if (!g_haveLast || !sameUid(uid, g_lastUid)) return true;
    return (now - g_lastPublishMs) >= FORGEKEY_BADGE_READER_DEBOUNCE_MS;
}

void recordPublished(const CardUid& uid, unsigned long now) {
    g_lastUid = uid;
    g_haveLast = true;
    g_lastPublishMs = now;
}

// Render a UID to uppercase hex with no separators, e.g. {04 A1 B2 C3 D4} ->
// "04A1B2C3D4". `out` must hold at least kMaxUidBytes*2 + 1 chars.
void formatUidHex(const CardUid& uid, char* out, size_t outLen) {
    static const char kHex[] = "0123456789ABCDEF";
    size_t n = uid.length;
    if (n > kMaxUidBytes) n = kMaxUidBytes;
    size_t pos = 0;
    for (size_t i = 0; i < n && pos + 2 < outLen; ++i) {
        out[pos++] = kHex[(uid.bytes[i] >> 4) & 0x0F];
        out[pos++] = kHex[uid.bytes[i] & 0x0F];
    }
    out[pos] = '\0';
}

// 16-hex-char replay-protection nonce from the hardware RNG.
String makeNonce() {
    char buf[17];
    snprintf(buf, sizeof(buf), "%08lx%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random());
    return String(buf);
}

void publishAccessRequest(const CardUid& uid) {
    char hex[kMaxUidBytes * 2 + 1];
    formatUidHex(uid, hex, sizeof(hex));

    // Payload shape is the authoritative forgekey.access_request.v1 contract
    // shared with OMS (see docs/contracts/mqtt.md and the schema). Device is a
    // pure sensor: credential + reader id only, no decision.
    JsonDocument doc;
    doc["schema_version"] = FORGEKEY_SCHEMA_ACCESS_REQUEST_V1;
    doc["credential_type"] = "badge";
    doc["credential_id"] = hex;
    doc["reader_id"] = g_reader->readerId();
    if (ForgeKeyTime::clockValid()) {
        doc["timestamp"] = (uint32_t)ForgeKeyTime::epochNow();
    }
    doc["nonce"] = makeNonce();

    String payload;
    serializeJson(doc, payload);
    bool ok = mqttClient.publishAccessRequest(payload.c_str());
    Serial.printf("[CAP/badge_reader] access_request uid=%s reader=%s published=%d\n",
                  hex, g_reader->readerId(), (int)ok);
}

}  // namespace

bool detectFn() {
    if (!BoardManifest::capabilityAllowed("badge_reader")) {
        Serial.println("[CAP/badge_reader] skipped: not allowed by board manifest");
        return false;
    }
    g_reader = createActiveReader();
    if (!g_reader) {
        Serial.println("[CAP/badge_reader] skipped: no reader driver compiled in");
        return false;
    }
    return true;
}

void setupFn() {
    g_ready = g_reader && g_reader->begin();
    Serial.printf("[CAP/badge_reader] setup ready=%d reader=%s debounce_ms=%lu\n",
                  (int)g_ready, g_ready ? g_reader->readerId() : "(none)",
                  (unsigned long)FORGEKEY_BADGE_READER_DEBOUNCE_MS);
}

void tickFn() {
    if (!g_ready) return;
    unsigned long now = millis();
    if (now - g_lastPollMs < FORGEKEY_BADGE_READER_POLL_INTERVAL_MS) return;
    g_lastPollMs = now;

    CardUid uid;
    if (!g_reader->poll(uid) || uid.length == 0) return;
    if (!shouldPublish(uid, now)) return;  // collapse repeated reads of one tap
    recordPublished(uid, now);
    publishAccessRequest(uid);
}

}  // namespace BadgeReader

REGISTER_CAPABILITY(badge_reader, "badge_reader",
                    BadgeReader::detectFn,
                    BadgeReader::setupFn,
                    BadgeReader::tickFn,
                    "access/request")

#else  // no reader driver compiled in — capability is absent on this build.

namespace BadgeReader {
// Intentionally empty: createActiveReader() is only referenced when a driver is
// compiled in, so no stub definition is required here (mirrors status_matrix's
// compile-out shape).
}

#endif
