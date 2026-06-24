#include "badge_reader.h"

// PN532-over-I2C reference driver. This is the documented default reader, but
// the final reader hardware is TBD (Ian to confirm). Everything PN532-specific
// is isolated here: swapping to RC522/Wiegand/etc. means adding a sibling driver
// translation unit, not touching the capability or the MQTT contract.
//
// MOCK takes precedence so a build that sets both flags stays hardware-free and
// there is never a duplicate createActiveReader() definition.
#if defined(FORGEKEY_BADGE_READER_PN532) && !defined(FORGEKEY_BADGE_READER_MOCK)

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PN532.h>

// I2C bus + control pins. Defaults are placeholders for the XIAO ESP32-S3 and
// are overridable per build env; confirm against the real wiring when the reader
// hardware is finalised. Pins are mirrored as manifest claims in
// board_manifest.cpp so the pin-conflict checker can see them.
#ifndef FORGEKEY_BADGE_READER_I2C_SDA
#define FORGEKEY_BADGE_READER_I2C_SDA 5
#endif
#ifndef FORGEKEY_BADGE_READER_I2C_SCL
#define FORGEKEY_BADGE_READER_I2C_SCL 6
#endif
#ifndef FORGEKEY_BADGE_READER_PN532_IRQ
#define FORGEKEY_BADGE_READER_PN532_IRQ 8
#endif
#ifndef FORGEKEY_BADGE_READER_PN532_RESET
#define FORGEKEY_BADGE_READER_PN532_RESET 9
#endif
// readPassiveTargetID blocks up to this long waiting for a card; keep it short
// so the main loop stays responsive.
#ifndef FORGEKEY_BADGE_READER_PN532_POLL_TIMEOUT_MS
#define FORGEKEY_BADGE_READER_PN532_POLL_TIMEOUT_MS 80
#endif
#ifndef FORGEKEY_BADGE_READER_ID
#define FORGEKEY_BADGE_READER_ID "main"
#endif

namespace BadgeReader {
namespace {

class Pn532Reader : public Reader {
public:
    Pn532Reader()
        : pn532_(FORGEKEY_BADGE_READER_PN532_IRQ,
                 FORGEKEY_BADGE_READER_PN532_RESET, &Wire) {}

    bool begin() override {
        Wire.begin(FORGEKEY_BADGE_READER_I2C_SDA, FORGEKEY_BADGE_READER_I2C_SCL);
        pn532_.begin();
        uint32_t version = pn532_.getFirmwareVersion();
        if (!version) {
            Serial.println("[CAP/badge_reader] PN532 not found on I2C — reader inactive");
            return false;
        }
        pn532_.SAMConfig();
        Serial.printf("[CAP/badge_reader] PN532 fw=0x%08lx sda=%d scl=%d irq=%d reset=%d\n",
                      (unsigned long)version,
                      FORGEKEY_BADGE_READER_I2C_SDA, FORGEKEY_BADGE_READER_I2C_SCL,
                      FORGEKEY_BADGE_READER_PN532_IRQ, FORGEKEY_BADGE_READER_PN532_RESET);
        return true;
    }

    bool poll(CardUid& out) override {
        uint8_t uid[kMaxUidBytes] = {0};
        uint8_t uidLen = 0;
        if (!pn532_.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen,
                                        FORGEKEY_BADGE_READER_PN532_POLL_TIMEOUT_MS)) {
            return false;
        }
        if (uidLen == 0) return false;
        if (uidLen > kMaxUidBytes) uidLen = kMaxUidBytes;
        memcpy(out.bytes, uid, uidLen);
        out.length = uidLen;
        return true;
    }

    const char* readerId() const override { return FORGEKEY_BADGE_READER_ID; }

private:
    Adafruit_PN532 pn532_;
};

Pn532Reader g_pn532Reader;

}  // namespace

Reader* createActiveReader() { return &g_pn532Reader; }

}  // namespace BadgeReader

#endif
