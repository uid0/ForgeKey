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
//        - 200 → PNG body, ETag header. Decode and paint.
//        - 304 → no body. Panel keeps its current paint, sleep.
//        - 404 → display row exists but is retired. Paint "retired" card.
//        - 409 → display unbound to an asset. Paint a QR linking to the
//                staff bind page so anyone with the panel + a phone can
//                pick the asset.
//   POST /api/forgekey/epaper/<display_id>/bind/
//        - Staff-JWT endpoint; firmware never calls it. The QR painted
//          on 409 sends staff to the mobile bind page that does.
//   POST /api/forgekey/epaper/<display_id>/battery/
//        - Body: {"percent": 0..100}
//        - 200 on persist; 400 malformed; 404 unknown id.
//
// The display_id is a UUID the firmware generates on first boot and
// keeps in NVS. The server auto-creates an unbound row on first
// contact with image.png, so a panel pulled fresh off the shelf just
// boots, picks a display_id, hits image.png, gets 409, paints the
// bind QR, and waits for staff.
//
// Battery telemetry note: the panel does NOT route a battery ADC line
// to the XIAO socket (verified against the Seeed driver-board schematic
// PDF). The firmware reports 100% as a placeholder until either Seeed
// publishes a battery-sense path or operators wire a divider onto the
// `BAT_4V2` net by hand. Operators rely on the on-board charger LEDs
// for visual "swap me" signalling.
//
// OTA note: ePaper skips MQTT but still participates in fleet OTA by
// polling a display_id-keyed HTTPS policy endpoint once per wake before
// fetching display content. The policy schema is the same JSON accepted on
// MQTT firmware dispatch for the other Arduino targets.

#if defined(FORGEKEY_EPAPER) && !defined(FORGEKEY_DISABLE_EPAPER)

#include "epaper_pm_capability.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <PNGdec.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_random.h>
#include <esp_sleep.h>
#include <qrcode.h>
#include <stdint.h>

// Seeed_GFX picks up the panel driver (UC8179) and the XIAO socket
// pin map from BOARD_SCREEN_COMBO=502 + USE_XIAO_EPAPER_DRIVER_BOARD
// defined in platformio.ini. Setup502 turns on `EPAPER_ENABLE` which
// brings in the `EPaper` class — different from TFT_eSPI even though
// they share the same header.
#include <TFT_eSPI.h>

#include "../capability.h"
#include "../../provisioning/device_config.h"
#include "../../ota/ota_updater.h"
#include "../../boards/board_manifest.h"
#include "../registry.h"

namespace EPaperPmCapability {

namespace {

// NVS namespace + keys. ESP-IDF NVS keys cap at 15 chars; the short
// "epaper" + 3-char suffixes leave room and stay readable in the
// `nvs_get_str` debug surfaces.
constexpr const char *kNvsNamespace = "epaper";
constexpr const char *kNvsKeyDisplayId = "did";
constexpr const char *kNvsKeyEtag = "etag";
constexpr const char *kNvsKeyUnchangedCount = "unch";
constexpr const char *kNvsKeyFailureCount = "fail";
constexpr const char *kNvsKeyBatteryPostSkips = "bskip";
constexpr const char *kNvsKeyWakeIntervalMin = "wake_min";

// Panel geometry. The Seeed_GFX driver reports these too, but they're
// the cleanest place to put the size assumptions the QR / PNG paint
// helpers rely on.
constexpr int kPanelWidth = 800;
constexpr int kPanelHeight = 480;

// PNG buffer cap. A 1-bit 800x480 PNG with the OMS render service's
// content (text + boxes) compresses to ~5-15KB; 64KB is comfortably
// above the high-water mark and well inside the ESP32-C3's 320KB SRAM.
// Capping prevents a runaway response from exhausting the heap.
constexpr size_t kMaxPngBytes = 65536;

// Adaptive cadence. ePaper keeps its image with no power, so repeated
// unchanged wakes should decay toward a quiet polling interval instead
// of spending battery on hourly WiFi + HTTP sessions forever.
constexpr uint32_t kMinWakeIntervalMin = 5;
constexpr uint32_t kMaxQuietWakeIntervalMin = 12 * 60;
constexpr uint32_t kErrorBaseWakeIntervalMin = 15;
constexpr uint32_t kMaxErrorWakeIntervalMin = 4 * 60;
constexpr uint32_t kSetupRetryWakeIntervalMin = 5;
constexpr uint32_t kRetiredWakeIntervalMin = 12 * 60;
constexpr size_t kMaxOtaPolicyBytes = 4096;

// The SKU 6416 board cannot report real battery voltage, so the battery
// endpoint is a low-value heartbeat for this hardware. Send it occasionally
// instead of paying for a second HTTP request on every wake. Default to this
// threshold on new firmware so upgraded panels report once, then decay.
constexpr uint32_t kBatteryPostEveryWakeCycles = 24;

// QR code parameters. Version 5 (37x37 modules) at ECC level M holds
// up to 106 bytes — fits our typical bind URL of ~90 chars with
// room for longer OMS_HOST values. Six-pixel modules render to a
// 222x222 QR which sits comfortably on the 800x480 panel.
constexpr uint8_t kQrVersion = 5;
constexpr uint8_t kQrModulePx = 6;
constexpr uint8_t kQrQuietModules = 4;

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
uint32_t g_consecutiveUnchanged = 0;
uint32_t g_consecutiveFailures = 0;
uint32_t g_batteryPostSkips = kBatteryPostEveryWakeCycles;
uint32_t g_configuredWakeIntervalMin = DEFAULT_WAKE_INTERVAL_MIN;
bool g_ranThisBoot = false;

// PNG-decode callback can't capture state, so the decoder writes
// straight into the namespace-global panel above.
PNG g_png;

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

String bindUrl(const String &displayId) {
    // Frontend bind page lives at the same origin as the API; the
    // mobile-friendly route is /forgekey/epaper/bind?did=<uuid>.
    return absUrl(("/forgekey/epaper/bind?did=" + displayId).c_str());
}

// ---- Display-id provisioning ---------------------------------------

// RFC 4122 v4 UUID built from esp_random(). Persisted to NVS on first
// boot so the same panel keeps its identity across reflashes that
// don't wipe nvs.
String generateDisplayId() {
    uint8_t bytes[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = esp_random();
        bytes[i + 0] = static_cast<uint8_t>(r);
        bytes[i + 1] = static_cast<uint8_t>(r >> 8);
        bytes[i + 2] = static_cast<uint8_t>(r >> 16);
        bytes[i + 3] = static_cast<uint8_t>(r >> 24);
    }
    bytes[6] = (bytes[6] & 0x0F) | 0x40;  // version 4
    bytes[8] = (bytes[8] & 0x3F) | 0x80;  // RFC 4122 variant
    char buf[37];
    snprintf(buf, sizeof(buf),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5],
             bytes[6], bytes[7],
             bytes[8], bytes[9],
             bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return String(buf);
}

void persistDisplayId(const String &did) {
    Preferences prefs;
    prefs.begin(kNvsNamespace, /*readonly=*/false);
    prefs.putString(kNvsKeyDisplayId, did);
    prefs.end();
}

// ---- Panel paint helpers -------------------------------------------

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

void paintRetiredCard() {
    paintMessageCard(
        "Panel retired",
        "OMS has marked this display as inactive.",
        "Move it back to active via the admin UI.");
}

void drawQrAt(QRCode *qr, int originX, int originY) {
    // Quiet zone (white border around the QR) is required for reliable
    // scanning. We've already filled the screen with white in the
    // caller, so we only need to leave kQrQuietModules worth of pixels
    // around the modules below.
    for (uint8_t y = 0; y < qr->size; y++) {
        for (uint8_t x = 0; x < qr->size; x++) {
            if (qrcode_getModule(qr, x, y)) {
                const int px = originX + x * kQrModulePx;
                const int py = originY + y * kQrModulePx;
                g_panel.fillRect(px, py, kQrModulePx, kQrModulePx, TFT_BLACK);
            }
        }
    }
}

// Card painted when the panel exists in OMS but has no asset bound
// to it yet. A camera-phone-readable QR points to the mobile bind
// page; below it we print the display_id in plain text so staff can
// type it in if the QR fails to scan (cracked panel etc.).
void paintBindQrCard(const String &displayId) {
    g_panel.setRotation(0);
    g_panel.fillScreen(TFT_WHITE);
    g_panel.setTextColor(TFT_BLACK, TFT_WHITE);

    g_panel.setTextSize(3);
    g_panel.drawString("Bind this panel", 40, 30);
    g_panel.setTextSize(2);
    g_panel.drawString("Scan with phone to pick an asset:", 40, 80);

    QRCode qr;
    uint8_t qrData[qrcode_getBufferSize(kQrVersion)];
    const String url = bindUrl(displayId);
    qrcode_initText(&qr, qrData, kQrVersion, ECC_MEDIUM, url.c_str());

    // Center the QR horizontally; sit it below the heading.
    const int qrPixels = qr.size * kQrModulePx;
    const int originX = (kPanelWidth - qrPixels) / 2;
    const int originY = 130;
    drawQrAt(&qr, originX, originY);

    // Footer: full URL + display_id so staff have a fallback if the
    // scan doesn't take. The URL also helps them spot a wrong host
    // config at a glance.
    const int footerY = originY + qrPixels + 30;
    g_panel.setTextSize(2);
    g_panel.drawString(url, 40, footerY);
    g_panel.drawString("display_id: " + displayId, 40, footerY + 30);
    g_panel.drawString("MAC: " + WiFi.macAddress(), 40, footerY + 60);

    g_panel.update();
}

// ---- PNG decode ----------------------------------------------------

// PNGdec hands us one scanline at a time. For the e-paper we don't
// care about color depth — we just threshold the green channel of an
// RGB565 conversion (g is the most-significant component for
// human-perceived brightness in 5-6-5). Anything brighter than the
// mid-point becomes white, everything else black.
int pngDrawCallback(PNGDRAW *pDraw) {
    uint16_t pixels[kPanelWidth];
    const int width = pDraw->iWidth < kPanelWidth ? pDraw->iWidth : kPanelWidth;
    g_png.getLineAsRGB565(pDraw, pixels, PNG_RGB565_BIG_ENDIAN, 0xFFFFFFFF);
    for (int x = 0; x < width; x++) {
        const uint16_t px = pixels[x];
        // RGB565: rrrrr gggggg bbbbb. Use G (6 bits) as brightness;
        // it's the most accurate single-channel proxy and avoids
        // costly per-pixel arithmetic.
        const uint8_t g6 = (px >> 5) & 0x3F;
        g_panel.drawPixel(x, pDraw->y, g6 > 31 ? TFT_WHITE : TFT_BLACK);
    }
    return 1;
}

bool paintFromPng(const uint8_t *buf, size_t len) {
    const int rc = g_png.openRAM(const_cast<uint8_t *>(buf),
                                 static_cast<int>(len),
                                 pngDrawCallback);
    if (rc != PNG_SUCCESS) {
        Serial.printf("[epaper] PNG openRAM failed (rc=%d)\n", rc);
        return false;
    }
    g_panel.setRotation(0);
    g_panel.fillScreen(TFT_WHITE);
    const int dec = g_png.decode(nullptr, 0);
    g_png.close();
    if (dec != PNG_SUCCESS) {
        Serial.printf("[epaper] PNG decode failed (rc=%d)\n", dec);
        return false;
    }
    g_panel.update();
    return true;
}


void postOtaStatus(const char *state, const char *version, int progress, const char *error) {
    if (WiFi.status() != WL_CONNECTED || g_displayId.length() == 0) return;
    HTTPClient http;
    const String url = absUrl(String("/api/forgekey/epaper/" + g_displayId + "/firmware/status/").c_str());
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    String payload = "{\"state\":\"";
    payload += state ? state : "";
    payload += "\"";
    if (version && *version) {
        payload += ",\"version\":\"";
        payload += version;
        payload += "\"";
    }
    if (progress >= 0 && progress <= 100) {
        payload += ",\"progress\":";
        payload += String(progress);
    }
    if (error && *error) {
        payload += ",\"error\":\"";
        payload += error;
        payload += "\"";
    }
    OtaUpdater::appendHealthJson(payload);
    BoardManifest::appendHealthJson(payload, CapabilityRegistry::head());
    payload += "}";
    const int code = http.POST(payload);
    http.end();
    if (code < 200 || code >= 300) {
        Serial.printf("[epaper] OTA status POST failed (code=%d)\n", code);
    }
}

void pollOtaPolicy() {
    if (WiFi.status() != WL_CONNECTED || g_displayId.length() == 0) return;
    otaUpdater.setStatusCallback(postOtaStatus);

    HTTPClient http;
    const String url = absUrl(String("/api/forgekey/epaper/" + g_displayId + "/firmware.json").c_str());
    http.begin(url);
    http.addHeader("Accept", "application/json");
    const int code = http.GET();
    if (code == 204 || code == 404) {
        http.end();
        Serial.printf("[epaper] no OTA policy (code=%d)\n", code);
        return;
    }
    if (code != 200) {
        http.end();
        Serial.printf("[epaper] OTA policy fetch failed (code=%d)\n", code);
        return;
    }
    const int contentLength = http.getSize();
    if (contentLength <= 0 || contentLength > static_cast<int>(kMaxOtaPolicyBytes)) {
        http.end();
        Serial.printf("[epaper] OTA policy length unusable (len=%d)\n", contentLength);
        return;
    }
    String body = http.getString();
    http.end();

    OtaUpdater::Spec spec;
    if (!otaUpdater.parse(reinterpret_cast<const uint8_t *>(body.c_str()), body.length(), spec)) {
        postOtaStatus("failed", "", -1, "parse_error");
        return;
    }
    if (spec.version == FORGEKEY_FIRMWARE_VERSION) {
        Serial.printf("[epaper] OTA policy already on version %s\n", spec.version.c_str());
        return;
    }
    String rejectReason;
    if (!otaUpdater.isPolicyAllowed(spec, rejectReason)) {
        Serial.printf("[epaper] OTA policy rejected: %s\n", rejectReason.c_str());
        postOtaStatus("rejected", spec.version.c_str(), -1, rejectReason.c_str());
        return;
    }
    postOtaStatus("received", spec.version.c_str(), -1, nullptr);
    otaUpdater.apply(spec);  // restarts on success
}

// ---- HTTP wake-cycle stages ----------------------------------------

// Returns one of:
//   "ok"          200, PNG decoded and panel updated.
//   "unchanged"   304, panel keeps its current paint.
//   "bind"        409, display exists but unbound — paint bind QR.
//   "retired"     404, display row is inactive — paint retired card.
//   "error"       transport / decode failure — keep current paint.
const char *fetchImage() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[epaper] skipping image fetch — WiFi not connected");
        return "error";
    }
    HTTPClient http;
    const String url = absUrl(
        String("/api/forgekey/epaper/" + g_displayId + "/image.png").c_str());
    http.begin(url);
    if (g_lastEtag.length() > 0) {
        http.addHeader("If-None-Match", String('"') + g_lastEtag + String('"'));
    }
    // ETag header bubbles up only if we list it explicitly — HTTPClient
    // drops unknown response headers to save RAM.
    const char *trackedHeaders[] = {"ETag"};
    http.collectHeaders(trackedHeaders, 1);

    const int code = http.GET();
    if (code == 304) {
        Serial.println("[epaper] image unchanged (304) — skipping redraw");
        http.end();
        return "unchanged";
    }
    if (code == 409) {
        Serial.println("[epaper] display unbound (409) — painting bind QR");
        http.end();
        return "bind";
    }
    if (code == 404) {
        Serial.println("[epaper] display retired (404)");
        http.end();
        return "retired";
    }
    if (code != 200) {
        Serial.printf("[epaper] image fetch failed (code=%d)\n", code);
        http.end();
        return "error";
    }

    String etag = http.header("ETag");
    if (etag.length() >= 2 && etag.startsWith("\"") && etag.endsWith("\"")) {
        etag = etag.substring(1, etag.length() - 1);
    }
    g_lastEtag = etag;

    const int contentLength = http.getSize();
    if (contentLength <= 0 || contentLength > static_cast<int>(kMaxPngBytes)) {
        Serial.printf("[epaper] PNG length unusable (len=%d, cap=%u)\n",
                      contentLength, static_cast<unsigned>(kMaxPngBytes));
        http.end();
        return "error";
    }

    uint8_t *body = static_cast<uint8_t *>(malloc(contentLength));
    if (body == nullptr) {
        Serial.printf("[epaper] PNG alloc failed (len=%d)\n", contentLength);
        http.end();
        return "error";
    }

    WiFiClient *stream = http.getStreamPtr();
    if (stream == nullptr) {
        Serial.println("[epaper] PNG stream null");
        free(body);
        http.end();
        return "error";
    }

    int read = 0;
    const unsigned long startMs = millis();
    while (read < contentLength && (millis() - startMs) < 15000UL) {
        const int avail = stream->available();
        if (avail <= 0) {
            delay(2);
            continue;
        }
        const int got = stream->read(body + read, contentLength - read);
        if (got > 0) {
            read += got;
        }
    }
    http.end();

    if (read != contentLength) {
        Serial.printf("[epaper] PNG short read (%d/%d)\n", read, contentLength);
        free(body);
        return "error";
    }

    const bool painted = paintFromPng(body, static_cast<size_t>(contentLength));
    free(body);
    return painted ? "ok" : "error";
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

uint32_t clampWakeInterval(uint32_t minutes) {
    if (minutes < kMinWakeIntervalMin) {
        return kMinWakeIntervalMin;
    }
    return minutes;
}

uint32_t saturatingDoubledInterval(uint32_t baseMinutes,
                                   uint32_t exponent,
                                   uint32_t maxMinutes) {
    uint32_t minutes = baseMinutes;
    for (uint32_t i = 0; i < exponent && minutes < maxMinutes; ++i) {
        if (minutes > maxMinutes / 2) {
            return maxMinutes;
        }
        minutes *= 2;
    }
    return minutes > maxMinutes ? maxMinutes : minutes;
}

void persistWakeState() {
    Preferences prefs;
    prefs.begin(kNvsNamespace, /*readonly=*/false);
    prefs.putString(kNvsKeyEtag, g_lastEtag);
    prefs.putUInt(kNvsKeyUnchangedCount, g_consecutiveUnchanged);
    prefs.putUInt(kNvsKeyFailureCount, g_consecutiveFailures);
    prefs.putUInt(kNvsKeyBatteryPostSkips, g_batteryPostSkips);
    prefs.end();
}

void loadFromNvs() {
    Preferences prefs;
    prefs.begin(kNvsNamespace, /*readonly=*/true);
    g_displayId = prefs.getString(kNvsKeyDisplayId, String(""));
    g_lastEtag = prefs.getString(kNvsKeyEtag, String(""));
    g_consecutiveUnchanged = prefs.getUInt(kNvsKeyUnchangedCount, 0);
    g_consecutiveFailures = prefs.getUInt(kNvsKeyFailureCount, 0);
    g_batteryPostSkips = prefs.getUInt(kNvsKeyBatteryPostSkips,
                                       kBatteryPostEveryWakeCycles);
    g_configuredWakeIntervalMin = clampWakeInterval(
        prefs.getUInt(kNvsKeyWakeIntervalMin, DEFAULT_WAKE_INTERVAL_MIN));
    prefs.end();
}

uint32_t nextWakeIntervalForResult(const char *result) {
    if (strcmp(result, "ok") == 0) {
        g_consecutiveUnchanged = 0;
        g_consecutiveFailures = 0;
        return g_configuredWakeIntervalMin;
    }
    if (strcmp(result, "unchanged") == 0) {
        g_consecutiveFailures = 0;
        const uint32_t exponent = g_consecutiveUnchanged;
        if (g_consecutiveUnchanged < UINT32_MAX) {
            ++g_consecutiveUnchanged;
        }
        return saturatingDoubledInterval(g_configuredWakeIntervalMin,
                                         exponent,
                                         kMaxQuietWakeIntervalMin);
    }
    if (strcmp(result, "bind") == 0) {
        g_consecutiveUnchanged = 0;
        g_consecutiveFailures = 0;
        return kSetupRetryWakeIntervalMin;
    }
    if (strcmp(result, "retired") == 0) {
        g_consecutiveUnchanged = 0;
        g_consecutiveFailures = 0;
        return kRetiredWakeIntervalMin;
    }

    g_consecutiveUnchanged = 0;
    const uint32_t exponent = g_consecutiveFailures;
    if (g_consecutiveFailures < UINT32_MAX) {
        ++g_consecutiveFailures;
    }
    return saturatingDoubledInterval(kErrorBaseWakeIntervalMin,
                                     exponent,
                                     kMaxErrorWakeIntervalMin);
}

bool isBatteryHeartbeatEligible(const char *result) {
    // Only spend the extra request after a successful image check. If the
    // GET failed, a second POST is likely to fail too and costs battery
    // without improving the displayed state. Bind/retired panels also do
    // not need placeholder battery telemetry while waiting for staff action.
    return strcmp(result, "ok") == 0 || strcmp(result, "unchanged") == 0;
}

bool shouldPostBatteryThisWake(const char *result) {
    if (!isBatteryHeartbeatEligible(result)) {
        return false;
    }
    if (g_batteryPostSkips >= kBatteryPostEveryWakeCycles) {
        return true;
    }
    ++g_batteryPostSkips;
    return g_batteryPostSkips >= kBatteryPostEveryWakeCycles;
}

void markBatteryPostAttempted() {
    g_batteryPostSkips = 0;
}

void requestDeepSleep(uint32_t minutes) {
    minutes = clampWakeInterval(minutes);
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
        g_displayId = generateDisplayId();
        persistDisplayId(g_displayId);
        Serial.printf("[epaper] generated new display_id=%s\n", g_displayId.c_str());
    } else {
        Serial.printf("[epaper] display_id=%s\n", g_displayId.c_str());
    }
}

void tickFn() {
    if (g_ranThisBoot) {
        return;
    }
    g_ranThisBoot = true;

    Serial.println("[epaper] starting wake cycle");
    pollOtaPolicy();
    const char *result = fetchImage();
    if (strcmp(result, "bind") == 0) {
        paintBindQrCard(g_displayId);
    } else if (strcmp(result, "retired") == 0) {
        paintRetiredCard();
    }
    // "ok", "unchanged", and "error" all leave the panel in its
    // current state (paintFromPng already flushed on success; 304 and
    // transport errors keep the prior render). Any non-transport response
    // proves the post-OTA image can boot, join WiFi, and talk to OMS, so it
    // is safe to mark a pending OTA slot valid before deep sleep.
    if (strcmp(result, "error") != 0) {
        otaUpdater.markStableIfPending();
    }

    const uint32_t nextWakeMinutes = nextWakeIntervalForResult(result);

    // See header — no ADC line on this board variant; placeholder until
    // either Seeed publishes a path or somebody wires a divider. Avoid
    // spending an extra HTTP POST on every wake for placeholder telemetry.
    if (shouldPostBatteryThisWake(result)) {
        Serial.println("[epaper] posting placeholder battery heartbeat");
        postBattery(kPlaceholderBatteryPercent);
        markBatteryPostAttempted();
    } else if (isBatteryHeartbeatEligible(result)) {
        Serial.printf("[epaper] skipping battery heartbeat (%u/%u wake cycles)\n",
                      g_batteryPostSkips,
                      kBatteryPostEveryWakeCycles);
    } else {
        Serial.printf("[epaper] skipping battery heartbeat for result=%s\n", result);
    }

    persistWakeState();
    requestDeepSleep(nextWakeMinutes);
}

}  // namespace EPaperPmCapability

REGISTER_CAPABILITY(epaper_pm, "epaper_pm",
                    EPaperPmCapability::detectFn,
                    EPaperPmCapability::setupFn,
                    EPaperPmCapability::tickFn,
                    "image")

#endif  // defined(FORGEKEY_EPAPER) && !defined(FORGEKEY_DISABLE_EPAPER)
