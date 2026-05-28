#ifndef FORGEKEY_CAPABILITIES_EPAPER_PM_H
#define FORGEKEY_CAPABILITIES_EPAPER_PM_H

#include <Arduino.h>

// XIAO 7.5" ePaper PM-display capability.
//
// One panel = one ESP32 + Seeed Wio E-Paper 7.5" combo bound to a single
// asset in OMS. Firmware lifecycle is asymmetric to the other ForgeKey
// devices: instead of staying online and ticking, the e-paper panel runs
// a single wake-cycle then deep-sleeps for FORGEKEY_EPAPER_WAKE_INTERVAL_MINUTES.
// During each wake-cycle:
//
//   1. WiFi connect (uses the shared wifi_setup / captive portal path).
//   2. HTTP GET /api/forgekey/epaper/<display_id>/image.png with
//      `If-None-Match: <last-etag-from-NVS>`. On 304, skip the redraw;
//      on 200, decode the PNG and push it to the panel.
//   3. Sample the LiPo voltage on ADC1_CH0 (GPIO 1), convert to percent,
//      HTTP POST /api/forgekey/epaper/<display_id>/battery/ with the
//      result. OMS captures a Sentry warning when the value crosses
//      FORGEKEY_EPAPER_LOW_BATTERY_PERCENT (default 20).
//   4. Persist the new ETag + sleep timestamp to NVS, request deep sleep.
//
// The display_id is the same UUID the OMS admin sees on the
// EPaperDisplay row; it's provisioned during device enrollment and stored
// in NVS under the "epaper_did" key. If unset, the capability logs once
// and returns without driving the panel.

namespace EPaperPmCapability {

// Default wake interval — overridable from build flags or NVS at runtime.
// 60 min hits a balance between freshness of the days-until-PM number
// and battery life on the LiPo cell. Lower it for high-cycle assets
// (e.g. machines on a 7-day filter cadence), raise it for monthly stuff.
static constexpr uint32_t DEFAULT_WAKE_INTERVAL_MIN = 60;

// Low-battery floor matching the OMS-side default
// (FORGEKEY_EPAPER_LOW_BATTERY_PERCENT in settings.py). Used as a
// secondary local guard: the firmware can flash a "BATTERY LOW" badge
// on the panel itself when the server alert is also firing, so a
// passing operator sees the panel asking to be swapped without
// needing to read Sentry first.
static constexpr uint8_t LOW_BATTERY_PERCENT = 20;

// Probes for the Seeed Wio E-Paper 7.5" panel on the configured SPI
// pins. Returns false on a missing/unresponsive panel so the registry
// can skip setup() on a board that was flashed with the e-paper env
// by mistake.
bool detectFn();

// Configure the panel + decoder library, load the persisted ETag /
// display_id from NVS, and arm the deep-sleep wake reason. Idempotent.
void setupFn();

// Runs the full wake cycle (HTTP GET image → render → POST battery →
// deep sleep request). Designed to be called once per main loop in a
// build that does not use the long-running tick model.
void tickFn();

}  // namespace EPaperPmCapability

#endif  // FORGEKEY_CAPABILITIES_EPAPER_PM_H
