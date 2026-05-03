// mmwave-presence capability stub. Reserves the slot in the registry; the
// detect() probe always returns false today so this contributes nothing at
// runtime. Once a 24/60 GHz mmWave sensor is wired in (Seeed MR24HPC1, LD2410,
// etc.), fill in the probe + driver. Suggested probe approaches:
//   - LD2410: open Serial1 at 256000 baud, send the magic-word query frame
//     (FD FC FB FA 02 00 FF 00 01 00 04 03 02 01) and look for the ack.
//     Datasheet: https://www.hlktech.net/index.php?id=988
//   - MR24HPC1: same pattern over UART at 115200; protocol manual at
//     https://wiki.seeedstudio.com/Radar_MR24HPC1/.
// Topic suffix would be "presence" (boolean + distance/speed payload).

#ifndef FORGEKEY_DISABLE_MMWAVE_PRESENCE

#include "../capability.h"

namespace MmwavePresence {

bool detectFn();
void setupFn();
void tickFn();

bool detectFn() {
    // TODO: probe UART for the configured mmWave module's heartbeat / ack.
    return false;
}

void setupFn() {
    // unreachable while detectFn() returns false
}

void tickFn() {
    // unreachable while detectFn() returns false
}

}  // namespace MmwavePresence

REGISTER_CAPABILITY(mmwave_presence, "mmwave_presence",
                    MmwavePresence::detectFn,
                    MmwavePresence::setupFn,
                    MmwavePresence::tickFn,
                    "presence")

#endif
