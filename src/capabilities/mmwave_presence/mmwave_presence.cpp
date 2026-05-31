// mmWave-presence capability. Activation is explicit and manifest-gated: the
// radar UART pins must be declared before firmware touches the bus. Topic
// suffix: "presence" (boolean + distance/speed payload).

#ifndef FORGEKEY_DISABLE_MMWAVE_PRESENCE

#include "../capability.h"
#include "../../boards/board_manifest.h"
#include <Arduino.h>

namespace MmwavePresence {

bool detectFn();
void setupFn();
void tickFn();

namespace {
bool g_configured = false;
}

bool detectFn() {
    g_configured = BoardManifest::mmwaveConfigured() &&
                   BoardManifest::capabilityAllowed("mmwave_presence");
    if (!g_configured) {
        Serial.println("[CAP/mmwave_presence] skipped: no radar UART in board manifest");
        return false;
    }
    return true;
}

void setupFn() {
    if (!g_configured) return;
    Serial1.begin(BoardManifest::mmwaveBaud(), SERIAL_8N1,
                  BoardManifest::mmwaveRxPin(), BoardManifest::mmwaveTxPin());
    Serial.printf("[CAP/mmwave_presence] UART rx=%d tx=%d baud=%lu\n",
                  BoardManifest::mmwaveRxPin(), BoardManifest::mmwaveTxPin(),
                  (unsigned long)BoardManifest::mmwaveBaud());
}

void tickFn() {
    if (!g_configured) return;
    while (Serial1.available() > 0) {
        (void)Serial1.read();
    }
}

}  // namespace MmwavePresence

REGISTER_CAPABILITY(mmwave_presence, "mmwave_presence",
                    MmwavePresence::detectFn,
                    MmwavePresence::setupFn,
                    MmwavePresence::tickFn,
                    "presence")

#endif
