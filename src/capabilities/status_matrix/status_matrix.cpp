#include "status_matrix.h"

#if defined(FORGEKEY_RGB_MATRIX_PIN) && !defined(FORGEKEY_DISABLE_STATUS_MATRIX)

#include <Adafruit_NeoPixel.h>
#include <string.h>
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
uint32_t g_manualColor = 0;
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

bool indicatorColor(const char* indicator, uint32_t& out) {
    if (!indicator || !*indicator) return false;
    if (strcmp(indicator, "auto") == 0 || strcmp(indicator, "clear") == 0) {
        out = 0;
        return true;
    }
    if (strcmp(indicator, "ok") == 0 ||
        strcmp(indicator, "available") == 0 ||
        strcmp(indicator, "green") == 0) {
        out = color(0, 96, 24);
        return true;
    }
    if (strcmp(indicator, "attention") == 0 ||
        strcmp(indicator, "warning") == 0 ||
        strcmp(indicator, "yellow") == 0) {
        out = color(112, 72, 0);
        return true;
    }
    if (strcmp(indicator, "error") == 0 ||
        strcmp(indicator, "critical") == 0 ||
        strcmp(indicator, "red") == 0) {
        out = color(112, 0, 0);
        return true;
    }
    if (strcmp(indicator, "busy") == 0 ||
        strcmp(indicator, "reserved") == 0 ||
        strcmp(indicator, "blue") == 0) {
        out = color(0, 40, 112);
        return true;
    }
    if (strcmp(indicator, "off") == 0) {
        out = color(0, 0, 0);
        return true;
    }
    return false;
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
        fill(g_manualColor);
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
    uint32_t c = 0;
    if (!indicatorColor(indicator, c)) return false;
    if (strcmp(indicator, "auto") == 0 || strcmp(indicator, "clear") == 0) {
        clearIndicator();
        return true;
    }
    g_manualIndicator = true;
    g_manualColor = c;
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
void clearIndicator() {}
}

#endif
