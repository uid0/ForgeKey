// XIAO 7.5" ePaper PM-display capability.
//
// Built into the `seeed_xiao_esp32s3_epaper` PlatformIO env only — the
// other build envs exclude `src/capabilities/epaper_pm/` via
// `build_src_filter`. Compiles to a stub on those builds via the
// FORGEKEY_EPAPER guard below.
//
// Contract with OMS (see backend/forgekey/views.py for the server side):
//
//   GET  /api/forgekey/epaper/<display_id>/image.png
//        - 200 → PNG body, ETag header. Decode and draw.
//        - 304 → no body. Panel keeps its current paint, save sleep.
//        - 404 → display row not provisioned. Treat as fatal-for-cycle.
//        - 409 → display unbound to an asset. Treat as fatal-for-cycle.
//   POST /api/forgekey/epaper/<display_id>/battery/
//        - Body: {"percent": 0..100}
//        - 200 on persist; 400 on malformed payload; 404 on unknown id.
//
// The display_id is stored in NVS at provisioning time. If it is not
// set, the capability logs once and exits so a freshly-flashed board
// without an OMS row doesn't burn cycles in a redraw loop.

#if defined(FORGEKEY_EPAPER) && !defined(FORGEKEY_DISABLE_EPAPER)

#include "epaper_pm_capability.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_sleep.h>

#include "../capability.h"
#include "../../provisioning/device_config.h"

namespace EPaperPmCapability {

namespace {

// NVS namespace + keys. Kept short — the ESP-IDF NVS key length cap is
// 15 chars, and "epaper" + a 3-char suffix fits comfortably.
constexpr const char *kNvsNamespace = "epaper";
constexpr const char *kNvsKeyDisplayId = "did";
constexpr const char *kNvsKeyEtag = "etag";

// Per-cycle state. The capability runs once per wake on this device
// class, so locals would also work — but keeping them at namespace
// scope lets the OTA capability (which can fire mid-cycle) inspect
// what stage we were in for log/telemetry purposes.
String g_displayId;
String g_lastEtag;
bool g_ranThisBoot = false;

// Read the LiPo cell on ADC1_CH0 (GPIO 1 on the Seeed XIAO ESP32-S3
// header). The XIAO board exposes a voltage divider that halves Vbat
// — the 2x multiplier inverts that. The 0..100 range is clamped at
// the ends so a slightly-over-3.0V reading (occasionally seen on
// freshly charged cells) doesn't surface as 102%.
uint8_t readBatteryPercent() {
    // ESP32 ADC raw range is 0..4095 at 12-bit, full-scale at 3.3V.
    const int raw = analogRead(1);
    const float vAdc = (raw * 3.3f) / 4095.0f;
    const float vBat = vAdc * 2.0f;
    // Linear approximation: 3.3V empty → 4.2V full. Not battery-curve
    // accurate, but good enough for "swap me" signalling on a panel
    // that only reports every wake cycle.
    const float pct = (vBat - 3.3f) / (4.2f - 3.3f) * 100.0f;
    if (pct < 0.0f) return 0;
    if (pct > 100.0f) return 100;
    return static_cast<uint8_t>(pct);
}

// Build the absolute URL for a given path against the configured OMS
// host (compile-time defines from device_config.h). HTTPS is implied
// — OMS_PORT defaults to 443 and the upstream HTTPClient picks the
// transport from the scheme. Override OMS_HOST/OMS_PORT via build
// flags in platformio.ini for per-environment builds.
String absUrl(const char *path) {
    String scheme = (OMS_PORT == 443) ? "https://" : "http://";
    String base = scheme + String(OMS_HOST);
    if (OMS_PORT != 443 && OMS_PORT != 80) {
        base += ":" + String(OMS_PORT);
    }
    return base + String(path);
}

// Returns true if the cycle should also push the freshly-rendered
// image to the panel. Stage 2 of the wake-cycle.
bool fetchAndRenderImage() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[epaper] skipping image fetch — WiFi not connected");
        return false;
    }
    HTTPClient http;
    String url = absUrl(String("/api/forgekey/epaper/" + g_displayId + "/image.png").c_str());
    http.begin(url);
    if (g_lastEtag.length() > 0) {
        // OMS-side EPaperDisplayImageView strips the surrounding quotes
        // so we wrap with the W3C-canonical form on the way out.
        http.addHeader("If-None-Match", String('"') + g_lastEtag + String('"'));
    }
    const int code = http.GET();
    if (code == 304) {
        Serial.println("[epaper] image unchanged (304) — skipping redraw");
        http.end();
        return false;
    }
    if (code != 200) {
        Serial.printf("[epaper] image fetch failed (code=%d) — keeping current paint\n", code);
        http.end();
        return false;
    }

    // Capture the new ETag for next-cycle short-circuiting before we
    // drain the body — once we read the stream the header API is
    // implementation-dependent in some HTTPClient versions.
    String etag = http.header("ETag");
    if (etag.length() >= 2 && etag.startsWith("\"") && etag.endsWith("\"")) {
        etag = etag.substring(1, etag.length() - 1);
    }
    g_lastEtag = etag;

    // TODO(epaper-pm-display, hardware-pass-1): wire the PNG decoder +
    // GxEPD2 driver here. Pseudocode for the hardware bring-up:
    //
    //   PNG png;
    //   png.openRAM(payload, len, drawScanlineCb);
    //   display.firstPage();
    //   do {
    //       png.decode(nullptr, 0);
    //   } while (display.nextPage());
    //   display.hibernate();
    //
    // Until the panel + lib are bench-tested we read the body to drain
    // the socket cleanly and trust the server told us a fresh ETag.
    WiFiClient *stream = http.getStreamPtr();
    if (stream) {
        while (stream->available()) {
            stream->read();
        }
    }
    http.end();
    return true;
}

bool postBattery(uint8_t percent) {
    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }
    HTTPClient http;
    String url = absUrl(String("/api/forgekey/epaper/" + g_displayId + "/battery/").c_str());
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    JsonDocument body;
    body["percent"] = percent;
    String payload;
    serializeJson(body, payload);
    const int code = http.POST(payload);
    http.end();
    if (code != 200) {
        Serial.printf("[epaper] battery POST failed (code=%d)\n", code);
        return false;
    }
    return true;
}

void persistEtag() {
    Preferences prefs;
    prefs.begin(kNvsNamespace, /*readonly=*/false);
    prefs.putString(kNvsKeyEtag, g_lastEtag);
    prefs.end();
}

void loadFromNvs() {
    Preferences prefs;
    prefs.begin(kNvsNamespace, /*readonly=*/true);
    g_displayId = prefs.getString(kNvsKeyDisplayId, String(""));
    g_lastEtag = prefs.getString(kNvsKeyEtag, String(""));
    prefs.end();
}

void requestDeepSleep() {
    uint32_t minutes = DEFAULT_WAKE_INTERVAL_MIN;
    // TODO: read FORGEKEY_EPAPER_WAKE_INTERVAL_MINUTES from NVS to let
    // the operator dashboard tune cadence per panel without a reflash.
    const uint64_t microseconds = static_cast<uint64_t>(minutes) * 60ULL * 1000000ULL;
    Serial.printf("[epaper] deep-sleeping for %u minute(s)\n", minutes);
    esp_sleep_enable_timer_wakeup(microseconds);
    esp_deep_sleep_start();
}

}  // namespace

bool detectFn() {
    // The e-paper env builds for one specific hardware combo; treat
    // the env flag itself as the presence probe. Real driver
    // initialisation happens in setupFn() so a missing panel still
    // logs cleanly instead of hard-faulting in detect().
    return true;
}

void setupFn() {
    loadFromNvs();
    if (g_displayId.length() == 0) {
        Serial.println(
            "[epaper] no display_id in NVS — provision the panel against OMS "
            "before flashing. Capability staying idle.");
        return;
    }
    Serial.printf("[epaper] bound to display_id=%s\n", g_displayId.c_str());

    // TODO(epaper-pm-display, hardware-pass-1): initialise the GxEPD2
    // driver here.
    //   display.init(115200, true, 2, false);
    //   display.setRotation(0);
}

void tickFn() {
    if (g_ranThisBoot || g_displayId.length() == 0) {
        return;
    }
    g_ranThisBoot = true;

    Serial.println("[epaper] starting wake cycle");
    const bool drew = fetchAndRenderImage();
    (void)drew;  // currently informational — used once the PNG path lands
    const uint8_t battery = readBatteryPercent();
    Serial.printf("[epaper] battery=%u%%\n", battery);
    if (battery < LOW_BATTERY_PERCENT) {
        Serial.printf(
            "[epaper] battery is below local low-battery floor (%u%%) — OMS will "
            "also alert via Sentry once the POST lands\n",
            LOW_BATTERY_PERCENT);
    }
    postBattery(battery);
    persistEtag();
    requestDeepSleep();
}

}  // namespace EPaperPmCapability

REGISTER_CAPABILITY(epaper_pm, "epaper_pm",
                    EPaperPmCapability::detectFn,
                    EPaperPmCapability::setupFn,
                    EPaperPmCapability::tickFn,
                    "image")

#endif  // defined(FORGEKEY_EPAPER) && !defined(FORGEKEY_DISABLE_EPAPER)
