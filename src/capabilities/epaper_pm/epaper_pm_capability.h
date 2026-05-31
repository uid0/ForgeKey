#ifndef FORGEKEY_CAPABILITIES_EPAPER_PM_H
#define FORGEKEY_CAPABILITIES_EPAPER_PM_H

#include <Arduino.h>

// Seeed XIAO 7.5" ePaper PM-display capability.
//
// One panel = one Seeed XIAO + 7.5" ePaper combo (SKU 6416) bound to a
// single asset in OMS. Firmware lifecycle is asymmetric to the other
// ForgeKey devices: instead of staying online and ticking, the e-paper
// panel runs a single wake-cycle then deep-sleeps for
// FORGEKEY_EPAPER_WAKE_INTERVAL_MINUTES. During each wake-cycle:
//
//   1. WiFi connect (uses the shared wifi_setup / captive portal path).
//   2. HTTP GET /api/forgekey/epaper/<display_id>/image.png with
//      `If-None-Match: <last-etag-from-NVS>`. On 304 skip the redraw;
//      on 200 decode the PNG and push it to the panel; on 404/409
//      paint a "display not bound" card.
//   3. Occasionally HTTP POST /api/forgekey/epaper/<display_id>/battery/
//      with a placeholder 100% — the SKU 6416 driver board does NOT
//      route a battery voltage-divider line to the XIAO socket, so a
//      real reading is impossible without a hardware mod. The endpoint
//      stays declared on the OMS side for future device classes, but the
//      firmware skips most placeholder POSTs to save battery.
//   4. Persist the new ETag/backoff counters to NVS, request deep sleep.
//
// The display_id is the same UUID the OMS admin sees on the
// EPaperDisplay row; it's provisioned during device enrollment and
// stored in NVS at namespace "epaper" key "did". If unset, the
// capability paints an "Awaiting provisioning" card with the device
// MAC so the bench operator can paste it into OMS and bind the panel
// before the next wake.

namespace EPaperPmCapability {

// Default wake interval — overridable from build flags or NVS at runtime.
// 60 min is the active cadence after changed content. Repeated 304
// unchanged responses back off from this value up to the firmware's quiet
// cap; HTTP/network failures use a shorter exponential retry ladder.
static constexpr uint32_t DEFAULT_WAKE_INTERVAL_MIN = 60;

// Probes for the Seeed XIAO 7.5" ePaper panel. The driver board is
// fixed-pin and the env builds for one specific hardware combo, so
// we treat the build flag as the presence probe — real init happens
// in setupFn() and a missing panel logs cleanly there rather than
// hard-faulting in detect().
bool detectFn();

// Configure the panel + decoder library, load the persisted ETag /
// display_id from NVS, and arm the deep-sleep wake reason. Idempotent.
void setupFn();

// Runs the full wake cycle (HTTP GET image → render as needed →
// occasional battery heartbeat → adaptive deep sleep request). Designed
// to be called once per main loop in a
// build that does not use the long-running tick model.
void tickFn();

}  // namespace EPaperPmCapability

#endif  // FORGEKEY_CAPABILITIES_EPAPER_PM_H
