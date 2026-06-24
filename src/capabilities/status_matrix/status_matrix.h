#ifndef FORGEKEY_CAPABILITIES_STATUS_MATRIX_H
#define FORGEKEY_CAPABILITIES_STATUS_MATRIX_H

#include <Arduino.h>
#include "../status_led/status_led.h"

namespace StatusMatrix {

// Render pattern for a manual (OMS-driven) indicator. The built-in device
// status animations (Booting/Connected/Error/...) and the identify override
// are separate and keep the precedence they already had in render().
enum class Pattern : uint8_t {
    Solid,      // steady fill at the scaled color
    Blink,      // toggle color <-> off, default 750ms period
    SlowBlink,  // toggle color <-> off, default 1500ms period
    Breathe,    // smooth fade off -> color -> off, default 2000ms period
    Off,        // extinguished (manual indicator active but dark)
};

// Resolved presentation for a manual indicator. Colors are logical 0-255 RGB;
// `brightness` is a 0-255 per-pixel scale applied on top of the color so it
// composes with the global matrix brightness (which stays a master dimmer).
// `periodMs == 0` means "use the pattern's default period".
struct IndicatorSpec {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t brightness = 255;
    Pattern pattern = Pattern::Solid;
    unsigned long periodMs = 0;
};

void requestState(StatusLed::State s);
void setIdentifyOverride(bool on);
void triggerMessageFlash();

// Legacy semantic-keyword entry point (ok/error/green/.../auto). Kept for
// back-compat; resolves the keyword to a spec and renders it. Returns false
// for an unknown keyword (caller rejects with unsupported_indicator).
bool setIndicator(const char* indicator, unsigned long durationMs);
// Explicit entry point: render an already-resolved color/brightness/pattern.
bool setIndicator(const IndicatorSpec& spec, unsigned long durationMs);
void clearIndicator();

// --- Pure resolution helpers (defined in every build, no hardware access) ---
// These let the command parser turn OMS fields into an IndicatorSpec without
// duplicating the keyword/color/pattern tables, and stay callable even in the
// no-matrix stub build.

// Map a semantic keyword to its default presentation. Returns false for an
// unknown keyword. Sets `clear=true` for the auto/clear keywords (the caller
// should call clearIndicator() instead of rendering the spec).
bool keywordSpec(const char* keyword, IndicatorSpec& spec, bool& clear);
// Parse an explicit color: a named color or "#RRGGBB" (also bare "RRGGBB").
// Returns false if unrecognized. Array [r,g,b] colors are handled by the
// caller, which has the JSON document.
bool parseColor(const char* name, uint8_t& r, uint8_t& g, uint8_t& b);
// Parse a brightness word ("low"/"high") to a 0-255 scale. Returns false for
// an unknown word. Integer brightness is handled by the caller.
bool parseBrightness(const char* word, uint8_t& out);
// Parse a pattern mode name. Returns false if unknown.
bool parsePattern(const char* name, Pattern& out);
// Canonical lowercase name for a pattern (for command acks / status reports).
const char* patternName(Pattern p);

}

#endif
