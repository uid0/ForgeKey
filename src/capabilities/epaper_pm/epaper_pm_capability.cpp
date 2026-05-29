// Seeed XIAO 7.5" ePaper PM-display capability.
//
// Built into the `seeed_xiao_epaper` PlatformIO env only — the other
// build envs exclude `src/capabilities/epaper_pm/` via
// `build_src_filter`, and the FORGEKEY_EPAPER guard below stubs the
// translation unit if it's pulled into another build by mistake.
//
// Hardware: Seeed XIAO 7.5" ePaper Panel, SKU 6416. 800x480 mono,
// UC8179 driver. Ships with a XIAO ESP32-C3. Pin map is fixed by the
// driver board and consumed by the Seeed_GFX library through the
// `USE_XIAO_EPAPER_DRIVER_BOARD` build flag in platformio.ini. We
// only touch `tft.*` here — the underlying SPI / D0..D10 wiring is
// the library's problem.
//
// Contract with OMS (see backend/forgekey/views.py for the server):
//
//   GET  /api/forgekey/epaper/<display_id>/image.png
//        - 200 → PNG body, ETag header. Decode and draw.
//        - 304 → no body. Panel keeps its current paint, sleep.
//        - 404 → display row not provisioned. Render "unprovisioned" card.
//        - 409 → display unbound to an asset. Render "unbound" card.
//   POST /api/forgekey/epaper/<display_id>/battery/
//        - Body: {"percent": 0..100}
//        - 200 on persist; 400 malformed; 404 unknown id.
//
// The display_id is stored in NVS at provisioning time. If it is not
// set, the capability paints a help card with the device's MAC so the
// staff member at the bench can paste it into the OMS admin and bind
// the panel to an asset before the next wake.
//
// Battery telemetry note: the panel does NOT route a battery ADC line
// to the XIAO socket (verified against the Seeed driver-board schematic
// PDF). The firmware reports 100% as a placeholder until either Seeed
// publishes a battery-sense path or operators wire a divider onto the
// `BAT_4V2` net by hand. Operators rely on the on-board charger LEDs
// for visual "swap me" signalling.

#if defined(FORGEKEY_EPAPER) && !defined(FORGEKEY_DISABLE_EPAPER)

#include "epaper_pm_capability.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_sleep.h>

// Seeed_GFX picks up the panel driver (UC8179) and the XIAO socket
// pin map from BOARD_SCREEN_COMBO=502 + USE_XIAO_EPAPER_DRIVER_BOARD
// defined in platformio.ini. Setup502 turns on `EPAPER_ENABLE` which
// brings in the `EPaper` class — different from TFT_eSPI even though
// they share the same header.
#include <TFT_eSPI.h>

#include "../capability.h"
#include "../../provisioning/device_config.h"

namespace EPaperPmCapability {

namespace {

// NVS namespace + keys. ESP-IDF NVS keys cap at 15 chars; the short
// "epaper" + 3-char suffixes leave room and stay readable in the
// `nvs_get_str` debug surfaces.
constexpr const char *kNvsNamespace = "epaper";
constexpr const char *kNvsKeyDisplayId = "did";
constexpr const char *kNvsKeyEtag = "etag";

// Single global display instance — Seeed_GFX's `EPaper` class drives
// the UC8179 panel via the same overall API as `TFT_eSPI` (fillScreen,
// setTextColor, drawString) plus an `update()` flush that flips the
// buffered render to the panel. Keep at namespace scope so the
// destructor runs cleanly if we ever add a teardown step.
EPaper g_panel;

// Per-cycle state. The capability runs once per wake on this device
// class, so locals would also work — but keeping these at namespace
// scope lets the OTA capability (which can fire mid-cycle) inspect
// what stage we were in for log / telemetry purposes.
String g_displayId;
String g_lastEtag;
bool g_ranThisBoot = false;

// XIAO 7.5" ePaper Panel SKU 6416 has no battery voltage-divider line
// broken out to the XIAO socket — see the schematic. Until somebody
// solders a divider onto `BAT_4V2`, the POST is a placeholder so the
// OMS row carries *some* telemetry rather than nothing.
constexpr uint8_t kPlaceholderBatteryPercent = 100;

// ---- URL helpers ---------------------------------------------------

String absUrl(const char *path) {
    String scheme = (OMS_PORT == 443) ? "https://" : "http://";
    String base = scheme + String(OMS_HOST);
    if (OMS_PORT != 443 && OMS_PORT != 80) {
        base += ":" + String(OMS_PORT);
    }
    return base + String(path);
}

// ---- Panel paint helpers -------------------------------------------

// Card painted when the panel has no display_id in NVS yet — gives
// the bench operator the MAC to paste into the OMS admin.
void paintUnprovisionedCard() {
    g_panel.setRotation(0);
    g_panel.fillScreen(TFT_WHITE);
    g_panel.setTextColor(TFT_BLACK, TFT_WHITE);
    g_panel.setTextSize(2);
    g_panel.drawString("ForgeKey ePaper", 40, 40);
    g_panel.setTextSize(3);
    g_panel.drawString("Awaiting provisioning", 40, 100);
    g_panel.setTextSize(2);
    g_panel.drawString("MAC:", 40, 200);
    g_panel.drawString(WiFi.macAddress(), 130, 200);
    g_panel.drawString("Add an EPaperDisplay row in OMS admin", 40, 260);
    g_panel.drawString("with this MAC, then re-flash the device_id", 40, 290);
    g_panel.drawString("to NVS at \"epaper/did\".", 40, 320);
    g_panel.update();
}

void paintMessageCard(const char *title, const char *line1, const char *line2 = nullptr) {
    g_panel.setRotation(0);
    g_panel.fillScreen(TFT_WHITE);
    g_panel.setTextColor(TFT_BLACK, TFT_WHITE);
    g_panel.setTextSize(3);
    g_panel.drawString(title, 40, 60);
    g_panel.setTextSize(2);
    g_panel.drawString(line1, 40, 160);
    if (line2 != nullptr) {
        g_panel.drawString(line2, 40, 200);
    }
    g_panel.drawString(WiFi.macAddress(), 40, 420);
    g_panel.update();
}

// ---- HTTP wake-cycle stages ----------------------------------------

// Stage 1: fetch the latest rendered PNG. The actual PNG → e-paper
// scanline path is the hardware-pass-2 follow-up; for hardware-pass-1
// we verify the HTTP round-trip and panel paint independently, then
// glue them together once we know both halves work.
//
// Returns one of:
//   "ok"          200 with body, body buffered into RAM (decode TODO).
//   "unchanged"   304, panel keeps its current paint.
//   "unprovisioned" 404 or 409 from OMS.
//   "error"       anything else; panel keeps its current paint.
const char *fetchImage() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[epaper] skipping image fetch — WiFi not connected");
        return "error";
    }
    HTTPClient http;
    String url = absUrl(String("/api/forgekey/epaper/" + g_displayId + "/image.png").c_str());
    http.begin(url);
    if (g_lastEtag.length() > 0) {
        http.addHeader("If-None-Match", String('"') + g_lastEtag + String('"'));
    }
    const int code = http.GET();
    if (code == 304) {
        Serial.println("[epaper] image unchanged (304) — skipping redraw");
        http.end();
        return "unchanged";
    }
    if (code == 404 || code == 409) {
        Serial.printf("[epaper] OMS reports display not bound (code=%d)\n", code);
        http.end();
        return "unprovisioned";
    }
    if (code != 200) {
        Serial.printf("[epaper] image fetch failed (code=%d) — keeping current paint\n", code);
        http.end();
        return "error";
    }

    String etag = http.header("ETag");
    if (etag.length() >= 2 && etag.startsWith("\"") && etag.endsWith("\"")) {
        etag = etag.substring(1, etag.length() - 1);
    }
    g_lastEtag = etag;

    // TODO(epaper-pm-display, hardware-pass-2): decode http.getStream()
    // through PNGdec into the e-paper frame buffer. Pseudocode:
    //
    //   PNG png;
    //   png.openStream(http.getStreamPtr(), [](PNGDRAW *d) {
    //       for (int x = 0; x < d->iWidth; ++x) {
    //           uint8_t v = d->pPixels[x];
    //           g_panel.drawPixel(x, d->y, v > 127 ? TFT_WHITE : TFT_BLACK);
    //       }
    //   });
    //   png.decode(nullptr, 0);
    //   g_panel.update();
    //
    // For pass-1 we drain the body to free the socket and paint a
    // sentinel "received" card so the bench operator sees the panel
    // change between cycles even before the PNG path lands.
    WiFiClient *stream = http.getStreamPtr();
    if (stream) {
        while (stream->available()) {
            stream->read();
        }
    }
    http.end();
    paintMessageCard(
        "Image fetched",
        "Server returned a fresh PNG.",
        "Decode path lands in pass-2.");
    return "ok";
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

// ---- NVS + sleep ---------------------------------------------------

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
    // TODO: read FORGEKEY_EPAPER_WAKE_INTERVAL_MINUTES from NVS so the
    // operator dashboard can tune cadence per panel without a reflash.
    const uint64_t microseconds = static_cast<uint64_t>(minutes) * 60ULL * 1000000ULL;
    Serial.printf("[epaper] deep-sleeping for %u minute(s)\n", minutes);
    esp_sleep_enable_timer_wakeup(microseconds);
    esp_deep_sleep_start();
}

}  // namespace

// ---- Capability lifecycle ------------------------------------------

bool detectFn() {
    // The e-paper env builds for one specific hardware combo; treat
    // the env flag itself as the presence probe. Real driver init
    // happens in setupFn() so a missing panel still logs cleanly
    // instead of hard-faulting in detect().
    return true;
}

void setupFn() {
    g_panel.begin();
    loadFromNvs();
    Serial.printf("[epaper] booted; mac=%s\n", WiFi.macAddress().c_str());
    if (g_displayId.length() == 0) {
        Serial.println("[epaper] no display_id in NVS; painting provisioning card");
        paintUnprovisionedCard();
        return;
    }
    Serial.printf("[epaper] bound to display_id=%s\n", g_displayId.c_str());
}

void tickFn() {
    if (g_ranThisBoot || g_displayId.length() == 0) {
        return;
    }
    g_ranThisBoot = true;

    Serial.println("[epaper] starting wake cycle");
    const char *result = fetchImage();
    if (strcmp(result, "unprovisioned") == 0) {
        paintMessageCard(
            "Display not bound",
            "OMS could not find or bind this display.",
            "Visit OMS admin and link it to an asset.");
    }

    // See header — no ADC line on this board variant; placeholder until
    // either Seeed publishes a path or somebody wires a divider.
    postBattery(kPlaceholderBatteryPercent);
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
