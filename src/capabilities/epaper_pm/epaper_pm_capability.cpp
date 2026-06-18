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
//   GET  /api/forgekey/epaper/<display_id>/desired.json
//        - 200 JSON desired state/commands; 204/404 means no changes.
//   POST /api/forgekey/epaper/<display_id>/health/
//        - Body: ePaper health, render, wake, HTTP, and battery telemetry.
//
// The display_id is a UUID the firmware generates on first boot and
// keeps in NVS. The server auto-creates an unbound row on first
// contact with image.png, so a panel pulled fresh off the shelf just
// boots, picks a display_id, hits image.png, gets 409, paints the
// bind QR, and waits for staff.
//
// Battery telemetry note: the panel does NOT route a battery ADC line
// to the XIAO socket (verified against the Seeed driver-board schematic
// PDF). The default firmware therefore reports an explicit
// power.battery.available=false health object. If a hardware spin or field mod
// wires BAT_4V2 through a divider, define FORGEKEY_BATTERY_ADC_PIN
// plus divider calibration build flags to enable voltage telemetry.
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
#include <WiFiManager.h>
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
#include "../../power/power_manager.h"
#include "../registry.h"
#include "../../mqtt/mqtt_client.h"

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
constexpr const char *kNvsKeyWakeIntervalMin = "wake_min";
constexpr const char *kNvsKeyRetired = "retired";
// Minutes the panel has been running its existing image since the last
// full-screen ghosting refresh. The whole point of the persisted counter
// is to survive deep sleep — globals get zeroed.
constexpr const char *kNvsKeyMinSinceFull = "min_full";
// The interval the prior wake scheduled us to sleep. Stored so this wake
// can add the right elapsed time to the ghosting counter — the configured
// wake-min isn't enough because the adaptive cadence can double it under
// long unchanged streaks.
constexpr const char *kNvsKeyLastSchedMin = "sched_min";

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
// E-paper ghosting builds up while the same image stays on screen. Once
// a day, do a full white→black→white sweep (each `update()` is a full
// panel refresh) before repainting the actual content — clears latent
// pixels and resets contrast. Operator can also force this immediately
// via desired.full_refresh / command "full_refresh".
constexpr uint32_t kFullRefreshIntervalMin = 24 * 60;
constexpr size_t kMaxOtaPolicyBytes = 4096;
constexpr size_t kMaxDesiredStateBytes = 4096;

// Health is the only per-wake POST. Battery fields ride in that payload,
// so the old placeholder battery-only heartbeat is retired.

// QR code parameters. The bind URL is encoded in byte mode because it includes
// lowercase URL/query characters. Version 9 with quartile error correction
// holds 130 bytes in byte mode, leaving headroom over the default 92-byte URL
// while keeping modules large enough for camera phones on the 7.5" panel.
constexpr uint8_t kQrVersion = 9;
constexpr uint8_t kQrEcc = ECC_QUARTILE;
constexpr size_t kQrMaxPayloadBytes = 130;
constexpr uint8_t kQrModulePx = 5;
constexpr uint8_t kQrQuietModules = 4;
constexpr int kQrOriginY = 108;
constexpr int kQrFooterGapPx = 22;
constexpr int kQrPixels = (4 * kQrVersion + 17) * kQrModulePx;
constexpr int kQrQuietPx = kQrQuietModules * kQrModulePx;

static_assert(kQrOriginY >= kQrQuietPx, "QR quiet zone must remain on panel");
static_assert(kQrOriginY + kQrPixels + kQrQuietPx < kPanelHeight,
              "QR quiet zone must fit on panel");

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
uint32_t g_configuredWakeIntervalMin = DEFAULT_WAKE_INTERVAL_MIN;
uint32_t g_lastScheduledWakeIntervalMin = DEFAULT_WAKE_INTERVAL_MIN;
uint32_t g_minutesSinceFullRefresh = 0;
int g_lastHttpStatus = 0;
String g_renderStatus = "boot";
bool g_retiredByCommand = false;
bool g_ranThisBoot = false;

// PNG-decode callback can't capture state, so the decoder writes
// straight into the namespace-global panel above.
PNG g_png;

// Battery sensing is handled by the shared power module and board manifest.
// The stock SKU 6416 has no battery sense route to the XIAO socket, so its
// manifest reports an unsupported ADC path until a hardware revision or field
// divider configures FORGEKEY_BATTERY_ADC_PIN and divider calibration macros.

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

// White → black → white sweep. Each `g_panel.update()` is a full panel
// refresh on the UC8179 (~3-5s and blocks until done), so no extra
// delays are needed between transitions. Caller is responsible for
// clearing the image-cache etag and refetching the content so the next
// `fetchImage()` repaints the actual work-order rather than returning
// 304 Not Modified and leaving the panel blank.
void runGhostingRefresh() {
    Serial.println("[epaper] running daily ghosting refresh (white→black→white)");
    g_panel.setRotation(0);
    g_panel.fillScreen(TFT_WHITE);
    g_panel.update();
    g_panel.fillScreen(TFT_BLACK);
    g_panel.update();
    g_panel.fillScreen(TFT_WHITE);
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

void paintRetiredCard() {
    paintMessageCard(
        "Panel retired",
        "OMS has marked this display as inactive.",
        "Move it back to active via the admin UI.");
}

void drawQrAt(QRCode *qr, int originX, int originY) {
    // Quiet zone (white border around the QR) is required for reliable
    // scanning. Clear it explicitly so future layout edits cannot bleed text
    // or stale panel state into the scanner's required border.
    const int quietPx = kQrQuietModules * kQrModulePx;
    g_panel.fillRect(originX - quietPx, originY - quietPx,
                     qr->size * kQrModulePx + quietPx * 2,
                     qr->size * kQrModulePx + quietPx * 2,
                     TFT_WHITE);
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
    if (url.length() > kQrMaxPayloadBytes) {
        paintMessageCard(
            "Bind this panel",
            "QR URL is too long for this firmware.",
            ("display_id: " + displayId).c_str());
        return;
    }
    if (qrcode_initText(&qr, qrData, kQrVersion, kQrEcc, url.c_str()) != 0) {
        paintMessageCard(
            "Bind this panel",
            "QR generation failed.",
            ("display_id: " + displayId).c_str());
        return;
    }

    // Center the QR horizontally; sit it below the heading.
    const int qrPixels = qr.size * kQrModulePx;
    const int originX = (kPanelWidth - qrPixels) / 2;
    const int originY = kQrOriginY;
    drawQrAt(&qr, originX, originY);

    // Footer: full URL + display_id so staff have a fallback if the
    // scan doesn't take. The URL also helps them spot a wrong host
    // config at a glance.
    const int footerY = originY + qrPixels + kQrFooterGapPx;
    g_panel.setTextSize(1);
    g_panel.drawString(url, 40, footerY);
    g_panel.setTextSize(2);
    g_panel.drawString("display_id: " + displayId, 40, footerY + 30);
    g_panel.drawString("MAC: " + WiFi.macAddress(), 40, footerY + 60);

    g_panel.update();
}


void paintIdentifyCard() {
    paintMessageCard(
        "Identify panel",
        "This ePaper display is being identified from OMS.",
        ("display_id: " + g_displayId).c_str());
}

void paintFactoryResetCard() {
    paintMessageCard(
        "Factory reset",
        "Clearing display identity and WiFi credentials.",
        "The setup portal will start after reboot.");
}

uint32_t clampWakeInterval(uint32_t minutes);

// ---- Desired-state / command polling -------------------------------

struct DesiredState {
    bool forceRefresh = false;
    bool fullRefresh = false;
    bool retire = false;
    bool unretire = false;
    bool identify = false;
    bool factoryReset = false;
    bool hasCommand = false;
    String commandId;
};

const char *commandName(const DesiredState &desired) {
    if (desired.factoryReset) return "factory_reset";
    if (desired.retire) return "retire";
    if (desired.unretire) return "unretire";
    if (desired.identify) return "identify";
    if (desired.fullRefresh) return "full_refresh";
    if (desired.forceRefresh) return "force_refresh";
    return "none";
}

void postCommandStatus(const DesiredState &desired, const char *state, const char *error = nullptr) {
    if (WiFi.status() != WL_CONNECTED || g_displayId.length() == 0) return;
    if (strcmp(commandName(desired), "none") == 0) return;
    if (!desired.hasCommand && desired.commandId.length() == 0) return;

    HTTPClient http;
    const String url = absUrl(String("/api/forgekey/epaper/" + g_displayId + "/command/status/").c_str());
    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    JsonDocument body;
    body["schema_version"] = FORGEKEY_SCHEMA_EPAPER_V1;
    body["command"] = commandName(desired);
    body["state"] = state;
    if (desired.commandId.length() > 0) body["command_id"] = desired.commandId;
    if (error && *error) body["error"] = error;
    String payload;
    serializeJson(body, payload);

    const int code = http.POST(payload);
    http.end();
    if (code < 200 || code >= 300) {
        Serial.printf("[epaper] command status POST failed (code=%d)\n", code);
    }
}

void applyWakeCadence(uint32_t wakeMin) {
    const uint32_t clamped = clampWakeInterval(wakeMin);
    if (clamped == g_configuredWakeIntervalMin) return;
    g_configuredWakeIntervalMin = clamped;
    Preferences prefs;
    prefs.begin(kNvsNamespace, /*readonly=*/false);
    prefs.putUInt(kNvsKeyWakeIntervalMin, g_configuredWakeIntervalMin);
    prefs.end();
    Serial.printf("[epaper] OMS wake_min set to %u minute(s)\n", g_configuredWakeIntervalMin);
}

const char *firstCommandString(JsonVariantConst src,
                               const char *key1,
                               const char *key2,
                               const char *key3 = nullptr,
                               const char *key4 = nullptr) {
    const char *keys[] = {key1, key2, key3, key4};
    for (const char *key : keys) {
        if (key == nullptr) continue;
        JsonVariantConst value = src[key];
        if (value.is<const char *>()) {
            const char *text = value.as<const char *>();
            if (text && *text) return text;
        }
    }
    return "";
}

void parseCommandObject(JsonVariantConst src, DesiredState &desired) {
    if (src.isNull()) return;
    desired.hasCommand = true;
    const char *id = firstCommandString(src, "id", "command_id");
    if (id && *id) desired.commandId = id;
    const char *name = firstCommandString(src, "name", "command", "cmd", "action");
    if (strcmp(name, "force_refresh") == 0 || strcmp(name, "refresh") == 0) {
        desired.forceRefresh = true;
    } else if (strcmp(name, "full_refresh") == 0 || strcmp(name, "ghosting_refresh") == 0) {
        desired.fullRefresh = true;
    } else if (strcmp(name, "retire") == 0) {
        desired.retire = true;
    } else if (strcmp(name, "unretire") == 0 || strcmp(name, "activate") == 0) {
        desired.unretire = true;
    } else if (strcmp(name, "identify") == 0) {
        desired.identify = true;
    } else if (strcmp(name, "factory_reset") == 0 || strcmp(name, "factory-reset") == 0) {
        desired.factoryReset = true;
    }
}

DesiredState pollDesiredState() {
    DesiredState desired;
    if (WiFi.status() != WL_CONNECTED || g_displayId.length() == 0) return desired;

    HTTPClient http;
    const String url = absUrl(String("/api/forgekey/epaper/" + g_displayId + "/desired.json").c_str());
    http.begin(url);
    http.addHeader("Accept", "application/json");
    const int code = http.GET();
    if (code == 204 || code == 404) {
        http.end();
        Serial.printf("[epaper] no desired-state changes (code=%d)\n", code);
        return desired;
    }
    if (code != 200) {
        http.end();
        Serial.printf("[epaper] desired-state fetch failed (code=%d)\n", code);
        return desired;
    }
    const int contentLength = http.getSize();
    if (contentLength > static_cast<int>(kMaxDesiredStateBytes)) {
        http.end();
        Serial.printf("[epaper] desired-state length unusable (len=%d)\n", contentLength);
        return desired;
    }
    const String body = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        Serial.printf("[epaper] desired-state JSON parse failed: %s\n", err.c_str());
        return desired;
    }

    JsonVariantConst root = doc.as<JsonVariantConst>();
    JsonVariantConst desiredObj = root;
    if (!root["desired"].isNull()) {
        desiredObj = root["desired"];
    }
    if (!desiredObj["wake_min"].isNull()) {
        applyWakeCadence(desiredObj["wake_min"].as<uint32_t>());
    }
    if (!desiredObj["force_refresh"].isNull() && desiredObj["force_refresh"].as<bool>()) {
        desired.forceRefresh = true;
    }
    if (!desiredObj["full_refresh"].isNull() && desiredObj["full_refresh"].as<bool>()) {
        desired.fullRefresh = true;
    }
    if (!desiredObj["retired"].isNull()) {
        if (desiredObj["retired"].as<bool>()) desired.retire = true;
        else desired.unretire = true;
    }
    if (!desiredObj["retire"].isNull() && desiredObj["retire"].as<bool>()) desired.retire = true;
    if (!desiredObj["identify"].isNull() && desiredObj["identify"].as<bool>()) desired.identify = true;
    if (!desiredObj["factory_reset"].isNull() && desiredObj["factory_reset"].as<bool>()) desired.factoryReset = true;
    const char *commandId = desiredObj["command_id"] | "";
    if (commandId && *commandId) {
        desired.commandId = commandId;
        desired.hasCommand = true;
    }

    JsonVariantConst command = root["command"];
    if (!command.isNull()) parseCommandObject(command, desired);
    JsonVariantConst commands = root["commands"];
    if (commands.is<JsonArrayConst>()) {
        JsonArrayConst commandArray = commands.as<JsonArrayConst>();
        if (commandArray.size() > 0) {
            parseCommandObject(commandArray[0], desired);
        }
    }

    if (strcmp(commandName(desired), "none") != 0) {
        Serial.printf("[epaper] desired command=%s id=%s\n",
                      commandName(desired), desired.commandId.c_str());
    }
    return desired;
}

void persistRetiredFlag(bool retired) {
    g_retiredByCommand = retired;
    Preferences prefs;
    prefs.begin(kNvsNamespace, /*readonly=*/false);
    prefs.putBool(kNvsKeyRetired, retired);
    prefs.end();
}

void clearImageCache() {
    g_lastEtag = "";
    g_consecutiveUnchanged = 0;
    Preferences prefs;
    prefs.begin(kNvsNamespace, /*readonly=*/false);
    prefs.remove(kNvsKeyEtag);
    prefs.putUInt(kNvsKeyUnchangedCount, 0);
    prefs.end();
}

void factoryResetAndRestart() {
    Preferences prefs;
    if (prefs.begin(kNvsNamespace, /*readonly=*/false)) {
        prefs.clear();
        prefs.end();
    }
    WiFiManager wm;
    wm.resetSettings();
    delay(250);
    ESP.restart();
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
    String payload = "{\"schema_version\":\"" FORGEKEY_SCHEMA_OTA_STATUS_V1 "\",\"state\":\"";
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

// Gate values for the wake-time OTA pre-flight. Skipping a check costs a
// pulled-from-config integer compare; aborting an in-flight flash from a
// dying battery bricks a panel until someone walks up with a USB cable, so
// the asymmetry justifies pre-checking.
//   - kOtaMinBatteryPercent: skip if the panel reports under this. Stock
//     SKU 6416 doesn't route battery to the XIAO socket, so this only fires
//     on hardware-modded panels (FORGEKEY_BATTERY_ADC_PIN). Unmeasurable
//     battery (mv < 0) falls through.
//   - kOtaMinRssiDbm: skip if WiFi signal is weaker than this. A weak link
//     mostly costs wall-clock during the multi-MB download, but a mid-flash
//     disconnect that times out triggers Update.abort() which leaves the
//     pending slot invalid and reboot rolls back — so we still want it
//     rare. -85 dBm is the marginal-to-unusable boundary for ESP32-C3.
static constexpr int kOtaMinBatteryPercent = 30;
static constexpr int kOtaMinRssiDbm = -85;

void pollOtaPolicy() {
    if (WiFi.status() != WL_CONNECTED || g_displayId.length() == 0) return;
    otaUpdater.setStatusCallback(postOtaStatus);

    // Battery floor — only applies when the build can actually read battery.
    const PowerManager::BatteryConfig &batt = BoardManifest::batteryConfig();
    const int batteryMv = PowerManager::batteryVoltageMv(batt);
    if (batteryMv >= 0) {
        const int batteryPct = PowerManager::batteryPercentFromMv(batteryMv, batt);
        if (batteryPct < kOtaMinBatteryPercent) {
            Serial.printf(
                "[epaper] OTA check skipped: battery %d%% < %d%% — bricking "
                "risk on a mid-flash power loss\n",
                batteryPct, kOtaMinBatteryPercent);
            return;
        }
    }

    // RSSI floor — a marginal link is fine for the small image fetch
    // (which is the next wake-cycle step) but risky for a multi-MB
    // firmware download. Skip and try again next wake.
    const int rssi = WiFi.RSSI();
    if (rssi != 0 && rssi < kOtaMinRssiDbm) {
        Serial.printf(
            "[epaper] OTA check skipped: RSSI %d dBm < %d dBm — multi-MB "
            "download likely to stall mid-flash\n",
            rssi, kOtaMinRssiDbm);
        return;
    }

    HTTPClient http;
    // Hits the OMS firmware-check endpoint added in oms PR #673. Passing
    // ?current=<running_version> lets the server immediately 204 us when
    // no update is staged for this display, avoiding the parse round-trip.
    const String url = absUrl(
        String("/api/forgekey/epaper/" + g_displayId + "/firmware-check/?current=" + FORGEKEY_FIRMWARE_VERSION).c_str());
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
        g_renderStatus = "wifi_error";
        g_lastHttpStatus = 0;
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
    g_lastHttpStatus = code;
    if (code == 304) {
        g_renderStatus = "unchanged";
        Serial.println("[epaper] image unchanged (304) — skipping redraw");
        http.end();
        return "unchanged";
    }
    if (code == 409) {
        g_renderStatus = "bind";
        Serial.println("[epaper] display unbound (409) — painting bind QR");
        http.end();
        return "bind";
    }
    if (code == 404) {
        g_renderStatus = "retired";
        Serial.println("[epaper] display retired (404)");
        http.end();
        return "retired";
    }
    if (code != 200) {
        g_renderStatus = "http_error";
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
        g_renderStatus = "stream_error";
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
    g_renderStatus = painted ? "rendered" : "render_failed";
    return painted ? "ok" : "error";
}

bool postHealth(const char *cycleResult) {
    if (WiFi.status() != WL_CONNECTED || g_displayId.length() == 0) {
        return false;
    }
    HTTPClient http;
    const String url = absUrl(String("/api/forgekey/epaper/" + g_displayId + "/health/").c_str());
    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    String payload = "{";
    payload += "\"schema_version\":\"" FORGEKEY_SCHEMA_EPAPER_V1 "\"";
    payload += ",\"display_id\":\"" + g_displayId + "\"";
    payload += ",\"firmware_version\":\"";
    payload += FORGEKEY_FIRMWARE_VERSION;
    payload += "\"";
    payload += ",\"last_image_etag\":";
    if (g_lastEtag.length() > 0) {
        JsonDocument etagDoc;
        etagDoc["etag"] = g_lastEtag;
        String etagJson;
        serializeJson(etagDoc["etag"], etagJson);
        payload += etagJson;
    } else {
        payload += "null";
    }
    payload += ",\"unchanged_count\":" + String(g_consecutiveUnchanged);
    payload += ",\"failure_count\":" + String(g_consecutiveFailures);
    payload += ",\"wake_interval_min\":" + String(g_lastScheduledWakeIntervalMin);
    payload += ",\"configured_wake_min\":" + String(g_configuredWakeIntervalMin);
    payload += ",\"min_since_full_refresh\":" + String(g_minutesSinceFullRefresh);
    payload += ",\"render_status\":\"" + g_renderStatus + "\"";
    payload += ",\"cycle_result\":\"";
    payload += cycleResult ? cycleResult : "";
    payload += "\"";
    payload += ",\"last_http_status\":" + String(g_lastHttpStatus);
    payload += ",\"retired\":" + String(g_retiredByCommand ? "true" : "false");

    OtaUpdater::appendHealthJson(payload);
    BoardManifest::appendHealthJson(payload, CapabilityRegistry::head());
    PowerManager::appendHealthJson(payload, BoardManifest::batteryConfig());
    payload += "}";

    const int code = http.POST(payload);
    http.end();
    if (code < 200 || code >= 300) {
        Serial.printf("[epaper] health POST failed (code=%d)\n", code);
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
    prefs.putUInt(kNvsKeyMinSinceFull, g_minutesSinceFullRefresh);
    prefs.putUInt(kNvsKeyLastSchedMin, g_lastScheduledWakeIntervalMin);
    prefs.end();
}

void loadFromNvs() {
    Preferences prefs;
    prefs.begin(kNvsNamespace, /*readonly=*/true);
    g_displayId = prefs.getString(kNvsKeyDisplayId, String(""));
    g_lastEtag = prefs.getString(kNvsKeyEtag, String(""));
    g_consecutiveUnchanged = prefs.getUInt(kNvsKeyUnchangedCount, 0);
    g_consecutiveFailures = prefs.getUInt(kNvsKeyFailureCount, 0);
    g_configuredWakeIntervalMin = clampWakeInterval(
        prefs.getUInt(kNvsKeyWakeIntervalMin, DEFAULT_WAKE_INTERVAL_MIN));
    g_lastScheduledWakeIntervalMin =
        prefs.getUInt(kNvsKeyLastSchedMin, g_configuredWakeIntervalMin);
    g_minutesSinceFullRefresh = prefs.getUInt(kNvsKeyMinSinceFull, 0);
    g_retiredByCommand = prefs.getBool(kNvsKeyRetired, false);
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

void requestDeepSleep(uint32_t minutes) {
    minutes = clampWakeInterval(minutes);
    const uint64_t microseconds = static_cast<uint64_t>(minutes) * 60ULL * 1000000ULL;
    Serial.printf("[epaper] deep-sleeping for %u minute(s)\n", minutes);
    PowerManager::setSleepPolicy("deep_sleep_adaptive");
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
    PowerManager::begin();
    PowerManager::setSleepPolicy("deep_sleep_adaptive");
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
    // The interval we just slept (loaded from NVS in setupFn) is the
    // elapsed time since the previous wake stamped the counter. Add it
    // before any command-driven branch so a force-reset or fullRefresh
    // both consume + reset the same accumulator.
    g_minutesSinceFullRefresh += g_lastScheduledWakeIntervalMin;
    pollOtaPolicy();
    DesiredState desired = pollDesiredState();
    postCommandStatus(desired, "received");

    const char *result = "error";
    if (desired.factoryReset) {
        g_renderStatus = "factory_reset";
        postCommandStatus(desired, "applied");
        paintFactoryResetCard();
        postHealth("factory_reset");
        factoryResetAndRestart();
        return;
    }
    if (desired.unretire) {
        persistRetiredFlag(false);
        postCommandStatus(desired, "applied");
    }
    if (desired.retire) {
        persistRetiredFlag(true);
        g_renderStatus = "retired";
        result = "retired";
        paintRetiredCard();
        postCommandStatus(desired, "applied");
    } else if (desired.identify) {
        g_renderStatus = "identify";
        result = "ok";
        paintIdentifyCard();
        postCommandStatus(desired, "applied");
    } else if (g_retiredByCommand) {
        g_renderStatus = "retired";
        result = "retired";
        paintRetiredCard();
    } else {
        if (desired.forceRefresh) {
            clearImageCache();
            postCommandStatus(desired, "applied");
        }
        const bool ghostingDue = desired.fullRefresh ||
                                 g_minutesSinceFullRefresh >= kFullRefreshIntervalMin;
        if (ghostingDue) {
            runGhostingRefresh();
            // The black/white sweep wiped what was on the panel, so the
            // server's etag no longer matches our actual paint state —
            // drop the cache to force a 200 + repaint instead of a 304
            // that would leave the panel white.
            clearImageCache();
            g_minutesSinceFullRefresh = 0;
            if (desired.fullRefresh) {
                postCommandStatus(desired, "applied");
            }
        }
        result = fetchImage();
        if (strcmp(result, "bind") == 0) {
            paintBindQrCard(g_displayId);
        } else if (strcmp(result, "retired") == 0) {
            paintRetiredCard();
        }
    }

    // "ok", "unchanged", command-rendered cards, and server-side retired/bind
    // states all prove the post-OTA image can boot, join WiFi, and talk to OMS,
    // so it is safe to mark a pending OTA slot valid before deep sleep.
    if (strcmp(result, "error") != 0) {
        otaUpdater.markStableIfPending();
    }

    const uint32_t nextWakeMinutes = nextWakeIntervalForResult(result);
    g_lastScheduledWakeIntervalMin = nextWakeMinutes;
    persistWakeState();
    postHealth(result);
    requestDeepSleep(nextWakeMinutes);
}

}  // namespace EPaperPmCapability

REGISTER_CAPABILITY(epaper_pm, "epaper_pm",
                    EPaperPmCapability::detectFn,
                    EPaperPmCapability::setupFn,
                    EPaperPmCapability::tickFn,
                    "image")

#endif  // defined(FORGEKEY_EPAPER) && !defined(FORGEKEY_DISABLE_EPAPER)
