#include "lock_device.h"
#include <WiFi.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#include "../provisioning/register.h"
#include "../security/oms_command_pubkey.h"

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

// ===== Signed command validation =====
static bool decodeBase64Url(const String& input, uint8_t* output, size_t outputCapacity,
                            size_t& outputLen) {
    String normalized = input;
    normalized.replace('-', '+');
    normalized.replace('_', '/');
    while (normalized.length() % 4) normalized += '=';

    size_t produced = 0;
    int rc = mbedtls_base64_decode(output, outputCapacity, &produced,
                                   reinterpret_cast<const unsigned char*>(normalized.c_str()),
                                   normalized.length());
    if (rc != 0) {
        return false;
    }
    outputLen = produced;
    return true;
}

static String normalizeMacForClaim(const String& mac) {
    String normalized = mac;
    normalized.replace(":", "");
    normalized.replace("-", "");
    normalized.toLowerCase();
    return normalized;
}

static String activeCommandPubKeyPem() {
    DeviceCredentials creds = provisioning.credentials();
    if (creds.commandPublicKeyPem.length() > 0) {
        return creds.commandPublicKeyPem;
    }
    return String(kOmsCommandPubKeyPem);
}

static bool verifyEs256Signature(const String& signingInput,
                                 const uint8_t* signature,
                                 size_t signatureLen) {
    if (!signature || signatureLen != 64) return false;

    String pubKeyPem = activeCommandPubKeyPem();
    if (pubKeyPem.length() == 0) return false;

    uint8_t digest[32];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    mbedtls_sha256_update(&sha,
                          reinterpret_cast<const unsigned char*>(signingInput.c_str()),
                          signingInput.length());
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int rc = mbedtls_pk_parse_public_key(
        &pk,
        reinterpret_cast<const unsigned char*>(pubKeyPem.c_str()),
        pubKeyPem.length() + 1);
    if (rc != 0) {
        Serial.printf("[LOCK] command pubkey parse failed: -0x%04x\n", -rc);
        mbedtls_pk_free(&pk);
        return false;
    }
    if (!mbedtls_pk_can_do(&pk, MBEDTLS_PK_ECKEY)) {
        Serial.println("[LOCK] command pubkey is not an EC key");
        mbedtls_pk_free(&pk);
        return false;
    }

    mbedtls_mpi r;
    mbedtls_mpi s;
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    rc = mbedtls_mpi_read_binary(&r, signature, 32);
    if (rc == 0) rc = mbedtls_mpi_read_binary(&s, signature + 32, 32);
    if (rc == 0) {
        mbedtls_ecp_keypair* keyPair = mbedtls_pk_ec(pk);
        rc = mbedtls_ecdsa_verify(&keyPair->grp, digest, sizeof(digest),
                                  &keyPair->Q, &r, &s);
    }
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_pk_free(&pk);
    return rc == 0;
}

bool validateJwt(const char* token, long timestamp, const char* expectedCmd) {
    if (!token || strlen(token) == 0) return false;
    String jwt(token);
    int firstDot = jwt.indexOf('.');
    int secondDot = jwt.indexOf('.', firstDot + 1);
    if (firstDot <= 0 || secondDot <= firstDot + 1) {
        Serial.println("[LOCK] signed command malformed");
        return false;
    }

    String payloadB64 = jwt.substring(firstDot + 1, secondDot);
    String signatureB64 = jwt.substring(secondDot + 1);
    String signingInput = jwt.substring(0, secondDot);

    uint8_t payload[512];
    size_t payloadLen = 0;
    if (!decodeBase64Url(payloadB64, payload, sizeof(payload) - 1, payloadLen)) {
        Serial.println("[LOCK] signed command payload decode failed");
        return false;
    }
    payload[payloadLen] = '\0';

    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, reinterpret_cast<const char*>(payload), payloadLen);
    if (err) {
        Serial.printf("[LOCK] signed command payload parse failed: %s\n", err.c_str());
        return false;
    }

    long claimTimestamp = doc["timestamp"] | timestamp;
    long expiry = doc["exp"] | 0L;
    const char* claimMac = doc["mac"] | "";
    const char* claimCmd = doc["cmd"] | "";

    long now = time(nullptr);
    if (claimTimestamp > 0 && now > 0) {
        long diff = labs(now - claimTimestamp);
        if (diff > FORGEKEY_LOCK_CMD_TIMESTAMP_TOLERANCE_S) {
            Serial.printf("[LOCK] signed command timestamp drift=%ld exceeds tolerance\n", diff);
            return false;
        }
    }
    if (expiry > 0 && now > 0 && now > expiry) {
        Serial.println("[LOCK] signed command expired");
        return false;
    }

    if (claimMac && *claimMac) {
        String expectedMac = normalizeMacForClaim(g_macAddress);
        String actualMac = normalizeMacForClaim(String(claimMac));
        if (expectedMac != actualMac) {
            Serial.printf("[LOCK] signed command MAC mismatch expected=%s got=%s\n",
                          expectedMac.c_str(), actualMac.c_str());
            return false;
        }
    }
    if (expectedCmd && *expectedCmd && claimCmd && *claimCmd &&
        String(claimCmd) != expectedCmd) {
        Serial.printf("[LOCK] signed command cmd mismatch: expected=%s got=%s\n",
                      expectedCmd, claimCmd);
        return false;
    }

    uint8_t signature[80];
    size_t signatureLen = 0;
    if (!decodeBase64Url(signatureB64, signature, sizeof(signature), signatureLen)) {
        Serial.println("[LOCK] signed command signature decode failed");
        return false;
    }
    if (!verifyEs256Signature(signingInput, signature, signatureLen)) {
        Serial.println("[LOCK] signed command signature verification failed");
        return false;
    }

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
                    g_lastTrigger = Trigger::SIGNED_COMMAND;
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

// Handle an incoming unlock command with signed token
bool handleUnlockCommand(const char* token, long timestamp) {
    if (!token || strlen(token) == 0) return false;

    if (!validateJwt(token, timestamp, "unlock")) {
        Serial.println("[LOCK] signed command validation failed");
        return false;
    }

    // Valid signed command: pulse the solenoid
    g_solenoidPulseStart = millis();
    g_solenoidPulsing = true;
    digitalWrite(FORGEKEY_LOCK_SOLENOID_PIN, FORGEKEY_LOCK_SOLENOID_ACTIVE);

    // Transition to UNLOCKED state
    g_state = State::UNLOCKED;
    g_lastTrigger = Trigger::SIGNED_COMMAND;
    g_unlockTime = millis();

    Serial.printf("[LOCK] unlocked via signed command (timestamp=%ld)\n", timestamp);
    return true;
}

}  // namespace LockDevice
