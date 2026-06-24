#include "status_matrix.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ---------------------------------------------------------------------------
// Pure resolution helpers — compiled into EVERY build (matrix and no-matrix)
// so the command parser can resolve OMS fields without touching hardware or
// duplicating the keyword/color tables.
// ---------------------------------------------------------------------------
namespace StatusMatrix {
namespace {

// Brightness presets as a fraction of full scale (epic: low ~12%, high ~90%).
constexpr uint8_t kBrightnessLow = 31;    // ~12% of 255
constexpr uint8_t kBrightnessHigh = 230;  // ~90% of 255

bool ieq(const char* a, const char* b) {
    return a && b && strcasecmp(a, b) == 0;
}

bool parseHexColor(const char* s, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (!s) return false;
    if (*s == '#') ++s;
    if (strlen(s) != 6) return false;
    for (int i = 0; i < 6; ++i) {
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    auto hx = [](const char* p) -> uint8_t {
        char buf[3] = {p[0], p[1], '\0'};
        return (uint8_t)strtol(buf, nullptr, 16);
    };
    r = hx(s);
    g = hx(s + 2);
    b = hx(s + 4);
    return true;
}

}  // namespace

bool parsePattern(const char* name, Pattern& out) {
    if (!name || !*name) return false;
    if (ieq(name, "solid")) { out = Pattern::Solid; return true; }
    if (ieq(name, "blink")) { out = Pattern::Blink; return true; }
    if (ieq(name, "slow_blink") || ieq(name, "slowblink")) { out = Pattern::SlowBlink; return true; }
    if (ieq(name, "breathe") || ieq(name, "breath")) { out = Pattern::Breathe; return true; }
    if (ieq(name, "off") || ieq(name, "none")) { out = Pattern::Off; return true; }
    return false;
}

const char* patternName(Pattern p) {
    switch (p) {
        case Pattern::Solid:     return "solid";
        case Pattern::Blink:     return "blink";
        case Pattern::SlowBlink: return "slow_blink";
        case Pattern::Breathe:   return "breathe";
        case Pattern::Off:       return "off";
    }
    return "solid";
}

bool parseBrightness(const char* word, uint8_t& out) {
    if (!word || !*word) return false;
    if (ieq(word, "low")) { out = kBrightnessLow; return true; }
    if (ieq(word, "high")) { out = kBrightnessHigh; return true; }
    if (ieq(word, "off")) { out = 0; return true; }
    if (ieq(word, "full") || ieq(word, "max")) { out = 255; return true; }
    return false;
}

bool parseColor(const char* name, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (!name || !*name) return false;
    if (*name == '#') return parseHexColor(name, r, g, b);

    // Named full-scale palette. Per-command brightness scales these down, and
    // the global matrix brightness (a master dimmer) scales again on show().
    struct Named { const char* n; uint8_t r, g, b; };
    static const Named kPalette[] = {
        {"red",     255, 0,   0},
        {"green",   0,   255, 0},
        {"blue",    0,   0,   255},
        {"yellow",  255, 200, 0},
        {"orange",  255, 90,  0},
        {"purple",  160, 0,   255},
        {"magenta", 255, 0,   255},
        {"pink",    255, 60,  120},
        {"cyan",    0,   255, 255},
        {"white",   255, 255, 255},
        {"off",     0,   0,   0},
        {"black",   0,   0,   0},
    };
    for (const auto& c : kPalette) {
        if (ieq(name, c.n)) { r = c.r; g = c.g; b = c.b; return true; }
    }
    // Tolerate a bare 6-hex string with no leading '#'.
    return parseHexColor(name, r, g, b);
}

bool keywordSpec(const char* keyword, IndicatorSpec& spec, bool& clear) {
    clear = false;
    if (!keyword || !*keyword) return false;
    if (ieq(keyword, "auto") || ieq(keyword, "clear")) { clear = true; return true; }

    spec = IndicatorSpec{};  // defaults: brightness 255, Solid

    // Legacy semantic keywords keep their exact prior rendering: pre-dimmed
    // RGB at full per-command brightness, solid. This preserves back-compat
    // for bare {"indicator":"ok"}-style commands.
    if (ieq(keyword, "ok") || ieq(keyword, "available") || ieq(keyword, "green")) {
        spec.r = 0; spec.g = 96; spec.b = 24; return true;
    }
    if (ieq(keyword, "attention") || ieq(keyword, "warning") || ieq(keyword, "yellow")) {
        spec.r = 112; spec.g = 72; spec.b = 0; return true;
    }
    if (ieq(keyword, "error") || ieq(keyword, "critical") || ieq(keyword, "red")) {
        spec.r = 112; spec.g = 0; spec.b = 0; return true;
    }
    if (ieq(keyword, "busy") || ieq(keyword, "reserved") || ieq(keyword, "blue")) {
        spec.r = 0; spec.g = 40; spec.b = 112; return true;
    }
    if (ieq(keyword, "off")) {
        spec.pattern = Pattern::Off; return true;
    }

    // New color keyword(s): full-scale palette at a sensible default brightness.
    if (ieq(keyword, "purple") || ieq(keyword, "magenta")) {
        parseColor(keyword, spec.r, spec.g, spec.b);
        spec.brightness = kBrightnessHigh;
        return true;
    }

    // New status aliases map through the OMS presentation table so a bare
    // {"indicator":"in_use"} still resolves to a full color/brightness/pattern.
    if (ieq(keyword, "in_use")) {  // green / high / solid
        parseColor("green", spec.r, spec.g, spec.b);
        spec.brightness = kBrightnessHigh;
        return true;
    }
    if (ieq(keyword, "unavailable")) {  // red / low / solid
        parseColor("red", spec.r, spec.g, spec.b);
        spec.brightness = kBrightnessLow;
        return true;
    }
    if (ieq(keyword, "classroom") || ieq(keyword, "class")) {  // purple / high / slow_blink
        parseColor("purple", spec.r, spec.g, spec.b);
        spec.brightness = kBrightnessHigh;
        spec.pattern = Pattern::SlowBlink;
        spec.periodMs = 1500;
        return true;
    }
    if (ieq(keyword, "locked_out")) {  // extinguished
        spec.pattern = Pattern::Off;
        return true;
    }
    return false;
}

}  // namespace StatusMatrix

#if defined(FORGEKEY_RGB_MATRIX_PIN) && !defined(FORGEKEY_DISABLE_STATUS_MATRIX)

#include <Adafruit_NeoPixel.h>
#include "../capability.h"
#include "../../boards/board_manifest.h"

namespace StatusMatrix {

bool detectFn();
void setupFn();
void tickFn();

namespace {

#ifndef FORGEKEY_RGB_MATRIX_WIDTH
#define FORGEKEY_RGB_MATRIX_WIDTH 6
#endif

#ifndef FORGEKEY_RGB_MATRIX_HEIGHT
#define FORGEKEY_RGB_MATRIX_HEIGHT 10
#endif

#ifndef FORGEKEY_RGB_MATRIX_COUNT
#define FORGEKEY_RGB_MATRIX_COUNT (FORGEKEY_RGB_MATRIX_WIDTH * FORGEKEY_RGB_MATRIX_HEIGHT)
#endif

#ifndef FORGEKEY_RGB_MATRIX_BRIGHTNESS
#define FORGEKEY_RGB_MATRIX_BRIGHTNESS 24
#endif

#ifndef FORGEKEY_RGB_MATRIX_TYPE
#define FORGEKEY_RGB_MATRIX_TYPE (NEO_GRB + NEO_KHZ800)
#endif

#ifndef BLINK_PERIOD_MS
#define BLINK_PERIOD_MS 750
#endif

Adafruit_NeoPixel g_pixels(FORGEKEY_RGB_MATRIX_COUNT,
                           FORGEKEY_RGB_MATRIX_PIN,
                           FORGEKEY_RGB_MATRIX_TYPE);

StatusLed::State g_state = StatusLed::State::Booting;
bool g_setupComplete = false;
bool g_identifyOverride = false;
bool g_messageFlashActive = false;
unsigned long g_messageFlashStartMs = 0;
unsigned long g_lastFrameMs = 0;
uint16_t g_frame = 0;
bool g_manualIndicator = false;
IndicatorSpec g_manualSpec;
unsigned long g_manualExpiresMs = 0;

uint32_t color(uint8_t r, uint8_t g, uint8_t b) {
    return g_pixels.Color(r, g, b);
}

uint32_t colorFor(StatusLed::State state) {
    switch (state) {
        case StatusLed::State::Booting:      return color(0, 32, 96);
        case StatusLed::State::Provisioning: return color(96, 56, 0);
        case StatusLed::State::Connected:    return color(0, 80, 16);
        case StatusLed::State::Degraded:     return color(96, 72, 0);
        case StatusLed::State::Ota:          return color(0, 72, 96);
        case StatusLed::State::Error:        return color(112, 0, 0);
        case StatusLed::State::Identify:     return color(80, 80, 96);
        case StatusLed::State::Retired:      return color(80, 0, 80);
        case StatusLed::State::FactoryReset: return color(112, 32, 0);
    }
    return color(0, 80, 16);
}

uint32_t dim(uint32_t c, uint8_t scale) {
    uint8_t r = (uint8_t)((uint8_t)(c >> 16) * scale / 255);
    uint8_t g = (uint8_t)((uint8_t)(c >> 8) * scale / 255);
    uint8_t b = (uint8_t)((uint8_t)c * scale / 255);
    return color(r, g, b);
}

// Color for the active manual spec, with the per-command brightness applied.
uint32_t scaledColor(const IndicatorSpec& s) {
    uint8_t r = (uint8_t)((uint16_t)s.r * s.brightness / 255);
    uint8_t g = (uint8_t)((uint16_t)s.g * s.brightness / 255);
    uint8_t b = (uint8_t)((uint16_t)s.b * s.brightness / 255);
    return color(r, g, b);
}

// Triangle fade 0 -> 255 -> 0 across one period, used for the breathe pattern.
uint8_t breatheLevel(unsigned long now, unsigned long period) {
    if (period < 2) period = 2;
    const unsigned long t = now % period;
    unsigned long half = period / 2;
    if (half == 0) half = 1;
    return t < half ? (uint8_t)(t * 255 / half)
                    : (uint8_t)(255 - (t - half) * 255 / half);
}

bool matrixAllowed() {
    return BoardManifest::capabilityAllowed("status_matrix");
}

bool onPhaseFor(StatusLed::State state, unsigned long now) {
    switch (state) {
        case StatusLed::State::Booting:      return (now / 100) % 2 == 0;
        case StatusLed::State::Provisioning: {
            const unsigned long cycle = now % 1600;
            return cycle < 300 ||
                   (cycle >= 400 && cycle < 700) ||
                   (cycle >= 800 && cycle < 900) ||
                   (cycle >= 1000 && cycle < 1300);
        }
        case StatusLed::State::Connected:    return (now % 3000) < 120;
        case StatusLed::State::Degraded:     return (now % 1000) < 180;
        case StatusLed::State::Ota:          return (now / 100) % 2 == 0;
        case StatusLed::State::Error:        return (now / 200) % 2 == 0;
        case StatusLed::State::Identify:     return (now / BLINK_PERIOD_MS) % 2 == 0;
        case StatusLed::State::Retired:      return true;
        case StatusLed::State::FactoryReset: return (now / 100) % 2 == 0;
    }
    return false;
}

void fill(uint32_t c) {
    for (uint16_t i = 0; i < g_pixels.numPixels(); ++i) {
        g_pixels.setPixelColor(i, c);
    }
}

// Render the active manual indicator according to its color/brightness/pattern.
void drawManual(unsigned long now) {
    const IndicatorSpec& s = g_manualSpec;
    const uint32_t base = scaledColor(s);
    switch (s.pattern) {
        case Pattern::Off:
            fill(color(0, 0, 0));
            break;
        case Pattern::Solid:
            fill(base);
            break;
        case Pattern::Blink:
        case Pattern::SlowBlink: {
            unsigned long period = s.periodMs ? s.periodMs
                : (s.pattern == Pattern::SlowBlink ? 1500UL : 750UL);
            unsigned long half = period / 2;
            if (half == 0) half = 1;
            const bool on = (now / half) % 2 == 0;
            fill(on ? base : color(0, 0, 0));
            break;
        }
        case Pattern::Breathe: {
            const unsigned long period = s.periodMs ? s.periodMs : 2000UL;
            fill(dim(base, breatheLevel(now, period)));
            break;
        }
    }
}

void drawConnected(unsigned long now) {
    const uint32_t base = colorFor(StatusLed::State::Connected);
    fill(dim(base, 28));
    if ((now % 3000) < 180) {
        fill(base);
    }
}

void drawOta() {
    fill(dim(colorFor(StatusLed::State::Ota), 18));
    const uint16_t lit = (g_frame % g_pixels.numPixels());
    g_pixels.setPixelColor(lit, colorFor(StatusLed::State::Ota));
    if (lit > 0) g_pixels.setPixelColor(lit - 1, dim(colorFor(StatusLed::State::Ota), 96));
}

void render() {
    if (!g_setupComplete) return;
    const unsigned long now = millis();
    StatusLed::State state = g_identifyOverride ? StatusLed::State::Identify : g_state;

    if (g_manualIndicator && g_manualExpiresMs != 0 &&
        (long)(now - g_manualExpiresMs) >= 0) {
        g_manualIndicator = false;
        g_manualExpiresMs = 0;
    }

    if (g_messageFlashActive && !g_identifyOverride) {
        if (now - g_messageFlashStartMs >= 500) {
            g_messageFlashActive = false;
        } else {
            fill((now / 100) % 2 == 0 ? color(96, 96, 96) : color(0, 0, 0));
            g_pixels.show();
            return;
        }
    }

    if (state == StatusLed::State::Ota) {
        drawOta();
    } else if (state == StatusLed::State::Error ||
               state == StatusLed::State::FactoryReset ||
               state == StatusLed::State::Retired) {
        fill(onPhaseFor(state, now) ? colorFor(state) : color(0, 0, 0));
    } else if (g_manualIndicator && !g_identifyOverride) {
        drawManual(now);
    } else if (state == StatusLed::State::Connected) {
        drawConnected(now);
    } else {
        fill(onPhaseFor(state, now) ? colorFor(state) : color(0, 0, 0));
    }
    g_pixels.show();
}

}  // namespace

void requestState(StatusLed::State s) {
    g_state = s;
}

void setIdentifyOverride(bool on) {
    g_identifyOverride = on;
}

void triggerMessageFlash() {
    if (!g_setupComplete || g_identifyOverride) return;
    g_messageFlashActive = true;
    g_messageFlashStartMs = millis();
}

bool setIndicator(const char* indicator, unsigned long durationMs) {
    IndicatorSpec spec;
    bool clear = false;
    if (!keywordSpec(indicator, spec, clear)) return false;
    if (clear) {
        clearIndicator();
        return true;
    }
    return setIndicator(spec, durationMs);
}

bool setIndicator(const IndicatorSpec& spec, unsigned long durationMs) {
    g_manualIndicator = true;
    g_manualSpec = spec;
    g_manualExpiresMs = durationMs > 0 ? millis() + durationMs : 0;
    render();
    return true;
}

void clearIndicator() {
    g_manualIndicator = false;
    g_manualExpiresMs = 0;
    render();
}

bool detectFn() {
    if (!matrixAllowed()) {
        Serial.println("[CAP/status_matrix] skipped: not allowed by board manifest");
        return false;
    }
    return true;
}

void setupFn() {
    g_pixels.begin();
    g_pixels.setBrightness(FORGEKEY_RGB_MATRIX_BRIGHTNESS);
    fill(color(0, 0, 0));
    g_pixels.show();
    g_setupComplete = true;
    render();
}

void tickFn() {
    const unsigned long now = millis();
    if (now - g_lastFrameMs < 33) return;
    g_lastFrameMs = now;
    ++g_frame;
    render();
}

}  // namespace StatusMatrix

REGISTER_CAPABILITY(status_matrix, "status_matrix",
                    StatusMatrix::detectFn,
                    StatusMatrix::setupFn,
                    StatusMatrix::tickFn,
                    nullptr)

#else

namespace StatusMatrix {
void requestState(StatusLed::State) {}
void setIdentifyOverride(bool) {}
void triggerMessageFlash() {}
bool setIndicator(const char*, unsigned long) { return false; }
bool setIndicator(const IndicatorSpec&, unsigned long) { return false; }
void clearIndicator() {}
}

#endif
