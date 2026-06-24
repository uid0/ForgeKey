#include "badge_reader.h"

// Mock reader driver: emits synthetic UIDs with no hardware, so the full
// access-request pipeline (poll -> debounce -> publish -> OMS) is exercisable in
// CI and on a bare dev board. Selected by FORGEKEY_BADGE_READER_MOCK; it takes
// precedence over the PN532 driver if both flags are set.
#if defined(FORGEKEY_BADGE_READER_MOCK)

#include <Arduino.h>

// Each synthetic card is "presented" (poll() returns its UID on every poll) for
// kPresentMs, then "absent" for kGapMs before the next card in the table. The
// present window is shorter than the capability debounce window so a whole
// presentation collapses to exactly one event; the gap is longer than it so the
// next presentation produces a fresh event. This makes the mock a faithful
// exercise of the debounce path without hardware.
#ifndef FORGEKEY_BADGE_READER_MOCK_PRESENT_MS
#define FORGEKEY_BADGE_READER_MOCK_PRESENT_MS 1500UL
#endif
#ifndef FORGEKEY_BADGE_READER_MOCK_GAP_MS
#define FORGEKEY_BADGE_READER_MOCK_GAP_MS 8000UL
#endif

namespace BadgeReader {
namespace {

// A small table of UIDs of varying lengths (4/5/7 bytes) to mirror real-world
// ISO 14443 type-A variety. The 5-byte entry matches the documented contract
// example "04A1B2C3D4".
const CardUid kMockCards[] = {
    {{0x04, 0xA1, 0xB2, 0xC3, 0xD4}, 5},
    {{0xDE, 0xAD, 0xBE, 0xEF}, 4},
    {{0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66}, 7},
};
constexpr size_t kMockCardCount = sizeof(kMockCards) / sizeof(kMockCards[0]);

class MockReader : public Reader {
public:
    bool begin() override {
        cycleMs_ = FORGEKEY_BADGE_READER_MOCK_PRESENT_MS + FORGEKEY_BADGE_READER_MOCK_GAP_MS;
        Serial.printf("[CAP/badge_reader] MOCK driver: %u cards, present=%lums gap=%lums "
                      "(serial: send any line to force an immediate scan)\n",
                      (unsigned)kMockCardCount,
                      (unsigned long)FORGEKEY_BADGE_READER_MOCK_PRESENT_MS,
                      (unsigned long)FORGEKEY_BADGE_READER_MOCK_GAP_MS);
        return true;
    }

    bool poll(CardUid& out) override {
        // Manual serial trigger: any input line forces an immediate one-shot
        // scan of the next card, independent of the schedule.
        if (Serial.available() > 0) {
            while (Serial.available() > 0) (void)Serial.read();
            out = kMockCards[serialIndex_ % kMockCardCount];
            serialIndex_++;
            return true;
        }

        unsigned long now = millis();
        unsigned long phase = now % cycleMs_;
        size_t card = (size_t)((now / cycleMs_) % kMockCardCount);
        if (phase < FORGEKEY_BADGE_READER_MOCK_PRESENT_MS) {
            out = kMockCards[card];  // card "present": returns UID every poll
            return true;
        }
        return false;  // card "absent"
    }

    const char* readerId() const override { return "mock"; }

private:
    unsigned long cycleMs_ = 1;
    size_t serialIndex_ = 0;
};

MockReader g_mockReader;

}  // namespace

Reader* createActiveReader() { return &g_mockReader; }

}  // namespace BadgeReader

#endif
