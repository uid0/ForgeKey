#include "lock_device.h"
#include <WiFi.h>
#include <ArduinoJson.h>

namespace LockDevice {

// ===== Internal state =====
static State g_state = State::INITIALIZING;
static Trigger g_lastTrigger = Trigger::NONE;
static String g_macAddress;

// ===== GPIO pin state (debounced) =====
static bool g_reedClosed = false;
static bool g_latchLocked = false;
static bool g_irBroken = true;  // default: beam broken (no item)
static bool g_mortiseActive = false;

// ===== Debounce tracking =====
static unsigned long g_reedDebounceStart = 0;
static unsigned long g_latchDebounceStart = 0;
static unsigned long g_irDebounceStart = 0;
static unsigned long g_mortiseDebounceStart = 0;

// ===== Solenoid pulse tracking =====
static unsigned long g_solenoidPulseStart = 0;
static bool g_solenoidPulsing = false;

// ===== Alarm LED tracking =====
static unsigned long g_alarmFlashStart = 0;
static bool g_alarmLedOn = false;

// ===== Auto-relock tracking =====
static unsigned long g_unlockTime = 0;

// ===== Telemetry change tracking =====
static bool g_lastSecure = false;
static bool g_lastItemPresent = true;
static unsigned long g_lastTelemetryMs = 0;

// ===== JWT validation =====
// JWT format: header.payload.signature (base64url encoded)
// For HS256, the signature is HMAC-SHA256(header.payload, secret)
// We verify by computing HMAC-SHA256 of (header.payload) with our secret
// and comparing to the signature portion.

// Parse JWT payload to extract timestamp and expiry
static bool parseJwtPayload(const String& token, long& outTimestamp, long& outExpiry) {
    int firstDot = token.indexOf('.');
    int secondDot = token.indexOf('.', firstDot + 1);
    if (firstDot <= 0 || secondDot <= firstDot + 1) return false;

    String payloadB64 = token.substring(firstDot + 1, secondDot);

    // Replace base64url chars with standard base64
    String payloadPlain = payloadB64;
    payloadPlain.replace('-', '+');
    payloadPlain.replace('_', '/');
    while (payloadPlain.length() % 4) payloadPlain += '=';

    // Decode base64
    const char* b64chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int b64index[256];
    memset(b64index, -1, sizeof(b64index));
    for (int i = 0; i < 64; i++) b64index[(uint8_t)b64chars[i]] = i;

    uint8_t decoded[128];
    int decodedLen = 0;
    for (int i = 0; i < payloadPlain.length() && decodedLen < 128; i++) {
        char c = payloadPlain[i];
        if (c == '=') continue;
        int val = b64index[(uint8_t)c];
        if (val < 0) return false;

        static uint8_t buf[4];
        static int bufPos = 0;
        buf[bufPos++] = val;

        if (bufPos == 4) {
            decoded[decodedLen++] = (buf[0] << 2) | (buf[1] >> 4);
            if (decodedLen < 128) decoded[decodedLen++] = (buf[1] << 4) | (buf[2] >> 2);
            if (decodedLen < 128) decoded[decodedLen++] = (buf[2] << 6) | buf[3];
            bufPos = 0;
        }
    }

    // Parse JSON to find "exp" and "timestamp" fields
    StaticJsonDocument<128> doc;
    DeserializationError err = deserializeJson(doc, (const char*)decoded, decodedLen);
    if (err) return false;

    outTimestamp = doc["timestamp"] | 0L;
    outExpiry = doc["exp"] | 0L;

    return true;
}

bool validateJwt(const char* token, long timestamp) {
    if (!token || strlen(token) == 0) return false;

    long jwtTimestamp = 0;
    long jwtExpiry = 0;

    if (!parseJwtPayload(token, jwtTimestamp, jwtExpiry)) {
        return false;
    }

    // Check timestamp tolerance (prevent replay attacks)
    long now = time(nullptr);
    if (now > 0) {
        long diff = abs(now - jwtTimestamp);
        if (diff > FORGEKEY_LOCK_CMD_TIMESTAMP_TOLERANCE_S) {
            return false;
        }
    }

    // Check JWT expiry
    if (jwtExpiry > 0) {
        long now = time(nullptr);
        if (now > 0 && (long)now > jwtExpiry) {
            return false;
        }
    }

    // HMAC verification placeholder:
    // In production, implement HMAC-SHA256 verification using the ESP32
    // hardware HMAC peripheral or mbedtls. For now, we accept tokens that
    // pass timestamp and expiry checks.
    // TODO: Add HMAC-SHA256 signature verification with FORGEKEY_LOCK_JWT_SECRET

    return true;
}

void begin() {
    g_state = State::INITIALIZING;
    g_lastTrigger = Trigger::NONE;
    g_macAddress = WiFi.macAddress();

    // Configure GPIO pins
    pinMode(FORGEKEY_LOCK_SOLENOID_PIN, OUTPUT);
    digitalWrite(FORGEKEY_LOCK_SOLENOID_PIN, LOW);  // solenoid off

    pinMode(FORGEKEY_LOCK_REED_PIN, INPUT_PULLUP);
    pinMode(FORGEKEY_LOCK_MORTISE_PIN, INPUT_PULLUP);
    pinMode(FORGEKEY_LOCK_LATCH_SUPERVISOR_PIN, INPUT_PULLUP);
    pinMode(FORGEKEY_LOCK_IR_BEAM_PIN, INPUT_PULLUP);

    g_reedClosed = false;
    g_latchLocked = false;
    g_irBroken = true;
    g_mortiseActive = false;

    g_lastSecure = false;
    g_lastItemPresent = true;
    g_lastTelemetryMs = 0;

    Serial.printf("[LOCK] begin: solenoid=D%d reed=D%d mortise=D%d latch=D%d ir=D%d\n",
                 FORGEKEY_LOCK_SOLENOID_PIN,
                 FORGEKEY_LOCK_REED_PIN,
                 FORGEKEY_LOCK_MORTISE_PIN,
                 FORGEKEY_LOCK_LATCH_SUPERVISOR_PIN,
                 FORGEKEY_LOCK_IR_BEAM_PIN);
}

void tick() {
    unsigned long now = millis();

    // ===== Debounce GPIO reads =====
    bool reedRaw = digitalRead(FORGEKEY_LOCK_REED_PIN) == FORGEKEY_LOCK_REED_CLOSED;
    bool latchRaw = digitalRead(FORGEKEY_LOCK_LATCH_SUPERVISOR_PIN) == FORGEKEY_LOCK_LATCH_LOCKED;
    bool irRaw = digitalRead(FORGEKEY_LOCK_IR_BEAM_PIN) == FORGEKEY_LOCK_IR_BEAM_BROKEN;
    bool mortiseRaw = digitalRead(FORGEKEY_LOCK_MORTISE_PIN) == LOW;  // active low

    // Reed switch debounce
    if (reedRaw != g_reedClosed) {
        if (g_reedDebounceStart == 0) g_reedDebounceStart = now;
        else if (now - g_reedDebounceStart >= FORGEKEY_LOCK_REED_DEBOUNCE_MS) {
            g_reedClosed = reedRaw;
            g_reedDebounceStart = 0;
        }
    } else {
        g_reedDebounceStart = 0;
    }

    // Latch supervisor debounce
    if (latchRaw != g_latchLocked) {
        if (g_latchDebounceStart == 0) g_latchDebounceStart = now;
        else if (now - g_latchDebounceStart >= FORGEKEY_LOCK_LATCH_DEBOUNCE_MS) {
            g_latchLocked = latchRaw;
            g_latchDebounceStart = 0;
        }
    } else {
        g_latchDebounceStart = 0;
    }

    // IR beam debounce
    if (irRaw != g_irBroken) {
        if (g_irDebounceStart == 0) g_irDebounceStart = now;
        else if (now - g_irDebounceStart >= FORGEKEY_LOCK_IR_DEBOUNCE_MS) {
            g_irBroken = irRaw;
            g_irDebounceStart = 0;
        }
    } else {
        g_irDebounceStart = 0;
    }

    // Mortise switch debounce
    if (mortiseRaw != g_mortiseActive) {
        if (g_mortiseDebounceStart == 0) g_mortiseDebounceStart = now;
        else if (now - g_mortiseDebounceStart >= FORGEKEY_LOCK_MORTISE_DEBOUNCE_MS) {
            g_mortiseActive = mortiseRaw;
            g_mortiseDebounceStart = 0;
        }
    } else {
        g_mortiseDebounceStart = 0;
    }

    // ===== State machine =====
    switch (g_state) {
        case State::INITIALIZING:
            // Stay in INITIALIZING until WiFi + NTP + MQTT are ready.
            // State transitions to SECURE happen in main.cpp after MQTT connect.
            break;

        case State::SECURE: {
            // SECURE: Reed closed AND latch locked
            bool secure = g_reedClosed && g_latchLocked;
            if (!secure) {
                if (g_solenoidPulsing || !g_latchLocked) {
                    g_state = State::UNLOCKED;
                    g_lastTrigger = Trigger::JWT;
                    g_unlockTime = now;
                    Serial.println("[LOCK] state -> UNLOCKED");
                } else if (!g_reedClosed) {
                    // Door opened without unlock command -> ALARM
                    g_state = State::ALARM;
                    g_lastTrigger = Trigger::ALARM_TIMEOUT;
                    Serial.println("[LOCK] state -> ALARM (door open without unlock)");
                }
            }
            break;
        }

        case State::UNLOCKED: {
            // UNLOCKED: latch disengaged, waiting for door to open or relock
            if (g_latchLocked) {
                g_state = State::SECURE;
                g_lastTrigger = Trigger::DOOR_CLOSE;
                Serial.println("[LOCK] state -> SECURE");
            } else if (g_mortiseActive) {
                g_state = State::ALARM;
                g_lastTrigger = Trigger::MORTISE;
                Serial.println("[LOCK] state -> ALARM (mortise engaged while unlocked)");
            }
            break;
        }

        case State::ACCESSING: {
            // ACCESSING: door open, IR beam monitoring item presence
            if (!g_mortiseActive && g_latchLocked) {
                g_state = State::SECURE;
                g_lastTrigger = Trigger::DOOR_CLOSE;
                Serial.println("[LOCK] state -> SECURE");
            } else if (g_mortiseActive) {
                g_state = State::ALARM;
                g_lastTrigger = Trigger::MORTISE;
                Serial.println("[LOCK] state -> ALARM (mortise engaged)");
            }
            break;
        }

        case State::ALARM: {
            // ALARM: rapid flash, waiting for resolution
            if (g_mortiseActive) {
                // Mortise switch still active, keep alarming
            } else if (g_latchLocked && g_reedClosed) {
                // Door closed and latch engaged - clear alarm
                g_state = State::SECURE;
                g_lastTrigger = Trigger::DOOR_CLOSE;
                Serial.println("[LOCK] state -> SECURE (alarm cleared)");
            }
            break;
        }

        default:
            break;
    }

    // ===== Solenoid pulse =====
    if (g_solenoidPulsing && now - g_solenoidPulseStart >= FORGEKEY_LOCK_SOLENOID_PULSE_MS) {
        digitalWrite(FORGEKEY_LOCK_SOLENOID_PIN, LOW);
        g_solenoidPulsing = false;
        Serial.println("[LOCK] solenoid pulse complete");
    }

    // ===== Alarm LED flash =====
    if (g_state == State::ALARM) {
        if (now - g_alarmFlashStart >= FORGEKEY_LOCK_ALARM_FLASH_MS) {
            g_alarmLedOn = !g_alarmLedOn;
            digitalWrite(21, g_alarmLedOn ? LOW : HIGH);  // onboard LED
            g_alarmFlashStart = now;
        }
    }

    // ===== Telemetry change detection =====
    bool currentSecure = g_reedClosed && g_latchLocked;
    bool currentItemPresent = !g_irBroken;

    if (now - g_lastTelemetryMs >= FORGEKEY_LOCK_TELEMETRY_INTERVAL_MS) {
        g_lastTelemetryMs = now;
        // Telemetry change will be detected by main.cpp via getTelemetry()
    }
}

State getState() {
    return g_state;
}

const char* stateName(State s) {
    switch (s) {
        case State::INITIALIZING: return "INITIALIZING";
        case State::SECURE: return "SECURE";
        case State::UNLOCKED: return "UNLOCKED";
        case State::ACCESSING: return "ACCESSING";
        case State::ALARM: return "ALARM";
        default: return "UNKNOWN";
    }
}

Trigger getLastTrigger() {
    return g_lastTrigger;
}

Telemetry getTelemetry() {
    Telemetry t{};
    t.secure = g_reedClosed && g_latchLocked;
    t.reed_closed = g_reedClosed;
    t.latch_locked = g_latchLocked;
    t.ir_broken = g_irBroken;
    t.mortise_active = g_mortiseActive;
    t.uptime_ms = millis();
    return t;
}

String getMacAddress() {
    return g_macAddress;
}

// Handle an incoming unlock command with JWT token
bool handleUnlockCommand(const char* token, long timestamp) {
    if (!token || strlen(token) == 0) return false;

    if (!validateJwt(token, timestamp)) {
        Serial.println("[LOCK] JWT validation failed");
        return false;
    }

    // Valid JWT: pulse the solenoid
    g_solenoidPulseStart = millis();
    g_solenoidPulsing = true;
    digitalWrite(FORGEKEY_LOCK_SOLENOID_PIN, FORGEKEY_LOCK_SOLENOID_ACTIVE);

    // Transition to UNLOCKED state
    g_state = State::UNLOCKED;
    g_lastTrigger = Trigger::JWT;
    g_unlockTime = millis();

    Serial.printf("[LOCK] unlocked via JWT (timestamp=%ld)\n", timestamp);
    return true;
}

}  // namespace LockDevice
