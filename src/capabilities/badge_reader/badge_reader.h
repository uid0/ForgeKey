#ifndef FORGEKEY_CAPABILITIES_BADGE_READER_H
#define FORGEKEY_CAPABILITIES_BADGE_READER_H

#include <Arduino.h>
#include <string.h>

// badge_reader — credential reader capability (SENSOR ONLY).
//
// The device reads an RFID/NFC credential and publishes a
// `forgekey.access_request.v1` event to OMS on forgekey/<mac>/access/request.
// It makes NO access decision and holds NO allowlist: OMS authorizes and drives
// the relay/lock/indicator over the existing command channel. This mirrors the
// server-authoritative cabinet_lock model.
//
// This header is the *reader abstraction* — the only surface the capability and
// MQTT logic depend on. The low-level read is isolated behind the Reader
// interface so a different reader (PN532 is the documented reference; RC522,
// Wiegand, etc.) can be dropped in by adding one driver translation unit that
// implements Reader + createActiveReader(), with no change to the capability or
// the MQTT contract.
//
// Driver selection is compile-time, behind mutually-exclusive build flags:
//   FORGEKEY_BADGE_READER_PN532  -> PN532 over I2C (reference hardware driver)
//   FORGEKEY_BADGE_READER_MOCK   -> synthetic-UID driver (CI / no hardware)
// When neither flag is set the capability compiles out to a stub (see the
// `#else` branch in badge_reader.cpp), exactly like status_matrix.

namespace BadgeReader {

// Largest raw UID across supported reader hardware. ISO 14443 UIDs are 4, 7, or
// 10 bytes; PN532 returns up to 7 for type A. 10 covers every documented case.
constexpr size_t kMaxUidBytes = 10;

// A credential UID read from the reader. `length` == 0 means "no card".
// `bytes` holds the raw UID big-endian as returned by the reader; the
// capability renders it to uppercase hex for `credential_id`.
struct CardUid {
    uint8_t bytes[kMaxUidBytes];
    uint8_t length;
};

// Reader hardware abstraction. The capability talks ONLY to this interface.
class Reader {
public:
    virtual ~Reader() = default;

    // Probe + initialise the reader. Returns true when a reader is present and
    // ready to poll. Called once from the capability setup() hook. May log.
    virtual bool begin() = 0;

    // Non-blocking poll. Returns true and fills `out` when a card UID was read
    // on this poll; returns false when no card is present. This is the
    // bool + out-param equivalent of the `optional<CardUid>` shape (std::optional
    // is avoided for pre-C++17 Arduino toolchains).
    virtual bool poll(CardUid& out) = 0;

    // Stable identifier for this reader/channel, emitted as `reader_id` in the
    // access-request event (default "main").
    virtual const char* readerId() const = 0;
};

// Returns the reader selected for this build, or nullptr when no reader driver
// is compiled in. Defined by exactly one driver translation unit
// (badge_reader_pn532.cpp or badge_reader_mock.cpp) selected by the build flag.
Reader* createActiveReader();

}  // namespace BadgeReader

#endif
