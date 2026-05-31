#include "command_validation.h"

#include "oms_command_pubkey.h"
#include "../provisioning/register.h"
#include "../time/time_sync.h"

#include <mbedtls/base64.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <stdlib.h>
#include <time.h>

namespace CommandValidation {
namespace {
constexpr size_t kReplayCacheSize = 16;
constexpr size_t kReplayValueMax = 72;
constexpr long kIssuedAtFutureSkewS = 300;
constexpr const char* kNvsNamespace = "cmdsec";
constexpr const char* kNvsCacheKey = "replay";

struct ReplayEntry {
    char commandId[kReplayValueMax];
    char nonce[kReplayValueMax];
};

ReplayEntry g_replayCache[kReplayCacheSize];
size_t g_replayCount = 0;
size_t g_replayNext = 0;
bool g_loaded = false;

bool nonEmpty(const char* value) {
    return value && value[0] != '\0';
}

bool copyBounded(char* dest, size_t destSize, const String& value) {
    if (value.length() == 0 || value.length() >= destSize) return false;
    snprintf(dest, destSize, "%s", value.c_str());
    return true;
}

String normalizeMac(String mac) {
    mac.replace(":", "");
    mac.replace("-", "");
    mac.toLowerCase();
    return mac;
}

bool decodeBase64Url(const String& input, uint8_t* output, size_t outputCapacity,
                     size_t& outputLen) {
    String normalized = input;
    normalized.replace('-', '+');
    normalized.replace('_', '/');
    while (normalized.length() % 4) normalized += '=';
    size_t produced = 0;
    int rc = mbedtls_base64_decode(output, outputCapacity, &produced,
                                   reinterpret_cast<const unsigned char*>(normalized.c_str()),
                                   normalized.length());
    if (rc != 0) return false;
    outputLen = produced;
    return true;
}

String activeCommandPubKeyPem() {
    DeviceCredentials creds = provisioning.credentials();
    if (creds.commandPublicKeyPem.length() > 0) return creds.commandPublicKeyPem;
    return String(kOmsCommandPubKeyPem);
}

bool verifyEs256RawSignature(const uint8_t* signingInput, size_t signingInputLen,
                             const uint8_t* signature, size_t signatureLen) {
    if (!signingInput || !signature || signatureLen != 64) return false;
    String pubKeyPem = activeCommandPubKeyPem();
    if (pubKeyPem.length() == 0) return false;

    uint8_t digest[32];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    mbedtls_sha256_update(&sha, signingInput, signingInputLen);
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int rc = mbedtls_pk_parse_public_key(
        &pk, reinterpret_cast<const unsigned char*>(pubKeyPem.c_str()), pubKeyPem.length() + 1);
    if (rc != 0 || !mbedtls_pk_can_do(&pk, MBEDTLS_PK_ECKEY)) {
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
        rc = mbedtls_ecdsa_verify(&keyPair->grp, digest, sizeof(digest), &keyPair->Q, &r, &s);
    }
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_pk_free(&pk);
    return rc == 0;
}

bool parseEpoch(JsonVariantConst value, time_t& out) {
    if (value.is<long>()) {
        out = static_cast<time_t>(value.as<long>());
        return out > 0;
    }
    if (!value.is<const char*>()) return false;
    const char* text = value.as<const char*>();
    if (!nonEmpty(text)) return false;
    char* end = nullptr;
    long numeric = strtol(text, &end, 10);
    if (end && *end == '\0' && numeric > 0) {
        out = static_cast<time_t>(numeric);
        return true;
    }
    struct tm tmValue = {};
    char* parsed = strptime(text, "%Y-%m-%dT%H:%M:%SZ", &tmValue);
    if (!parsed || *parsed != '\0') return false;
    out = mktime(&tmValue);
    return out > 0;
}

bool replaySeen(const String& commandId, const String& nonce) {
    for (size_t i = 0; i < g_replayCount; ++i) {
        if ((commandId.length() && commandId == g_replayCache[i].commandId) ||
            (nonce.length() && nonce == g_replayCache[i].nonce)) {
            return true;
        }
    }
    return false;
}

void persistReplayCache() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    String encoded;
    encoded.reserve(512);
    for (size_t i = 0; i < g_replayCount; ++i) {
        if (i) encoded += ';';
        encoded += g_replayCache[i].commandId;
        encoded += ',';
        encoded += g_replayCache[i].nonce;
    }
    nvs_set_str(handle, kNvsCacheKey, encoded.c_str());
    nvs_commit(handle);
    nvs_close(handle);
}

void loadReplayCache() {
    if (g_loaded) return;
    g_loaded = true;
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    size_t len = 0;
    if (nvs_get_str(handle, kNvsCacheKey, nullptr, &len) != ESP_OK || len == 0 || len > 1024) {
        nvs_close(handle);
        return;
    }
    char* buf = static_cast<char*>(calloc(len, 1));
    if (!buf) {
        nvs_close(handle);
        return;
    }
    if (nvs_get_str(handle, kNvsCacheKey, buf, &len) == ESP_OK) {
        char* save = nullptr;
        for (char* token = strtok_r(buf, ";", &save);
             token && g_replayCount < kReplayCacheSize;
             token = strtok_r(nullptr, ";", &save)) {
            char* comma = strchr(token, ',');
            if (!comma) continue;
            *comma = '\0';
            snprintf(g_replayCache[g_replayCount].commandId,
                     sizeof(g_replayCache[g_replayCount].commandId), "%s", token);
            snprintf(g_replayCache[g_replayCount].nonce,
                     sizeof(g_replayCache[g_replayCount].nonce), "%s", comma + 1);
            g_replayCount++;
        }
        g_replayNext = g_replayCount % kReplayCacheSize;
    }
    free(buf);
    nvs_close(handle);
}

String canonicalSigningInput(JsonVariantConst doc) {
    String input;
    input.reserve(192);
    input += doc["cmd"].as<const char*>();
    input += '\n';
    input += doc["command_id"].as<const char*>();
    input += '\n';
    input += doc["issued_at"].as<const char*>();
    input += '\n';
    input += doc["expires_at"].as<const char*>();
    input += '\n';
    input += doc["nonce"].as<const char*>();
    input += '\n';
    input += doc["actor"].as<const char*>();
    return input;
}

bool validateJwt(JsonVariantConst doc, const String& deviceMac, const char* token, bool challengeFlow, const char*& error) {
    String jwt(token);
    int firstDot = jwt.indexOf('.');
    int secondDot = jwt.indexOf('.', firstDot + 1);
    if (firstDot <= 0 || secondDot <= firstDot + 1) {
        error = "malformed_jwt";
        return false;
    }

    String payloadB64 = jwt.substring(firstDot + 1, secondDot);
    String signatureB64 = jwt.substring(secondDot + 1);
    String signingInput = jwt.substring(0, secondDot);

    uint8_t payload[768];
    size_t payloadLen = 0;
    if (!decodeBase64Url(payloadB64, payload, sizeof(payload) - 1, payloadLen)) {
        error = "malformed_jwt";
        return false;
    }
    payload[payloadLen] = '\0';

    StaticJsonDocument<768> claims;
    if (deserializeJson(claims, reinterpret_cast<const char*>(payload), payloadLen)) {
        error = "malformed_jwt";
        return false;
    }

    const char* claimCmd = claims["cmd"] | "";
    const char* claimCommandId = claims["command_id"] | "";
    if (!nonEmpty(claimCommandId)) claimCommandId = claims["jti"] | "";
    const char* claimNonce = claims["nonce"] | "";
    const char* claimActor = claims["actor"] | "";
    if (!nonEmpty(claimActor)) claimActor = claims["sub"] | "";
    const char* claimMac = claims["mac"] | "";
    if (!nonEmpty(claimMac)) claimMac = claims["device"] | "";
    if (strcmp(claimCmd, doc["cmd"].as<const char*>()) != 0 ||
        strcmp(claimCommandId, doc["command_id"].as<const char*>()) != 0 ||
        strcmp(claimNonce, doc["nonce"].as<const char*>()) != 0 ||
        (nonEmpty(claimActor) && strcmp(claimActor, doc["actor"].as<const char*>()) != 0)) {
        error = "jwt_claim_mismatch";
        return false;
    }
    if (nonEmpty(claimMac) && normalizeMac(String(claimMac)) != normalizeMac(deviceMac)) {
        error = "wrong_device";
        return false;
    }

    time_t now = ForgeKeyTime::epochNow();
    time_t exp = 0;
    if (parseEpoch(claims["exp"], exp)) {
        if (!ForgeKeyTime::clockValid() && !challengeFlow) {
            error = "clock_invalid";
            return false;
        }
        if (ForgeKeyTime::clockValid() && now > exp) {
            error = "expired";
            return false;
        }
    }

    uint8_t signature[80];
    size_t signatureLen = 0;
    if (!decodeBase64Url(signatureB64, signature, sizeof(signature), signatureLen) ||
        !verifyEs256RawSignature(reinterpret_cast<const uint8_t*>(signingInput.c_str()),
                                 signingInput.length(), signature, signatureLen)) {
        error = "invalid_signature";
        return false;
    }
    return true;
}

bool validateDetachedSignature(JsonVariantConst doc, const char* signatureText) {
    uint8_t signature[80];
    size_t signatureLen = 0;
    if (!decodeBase64Url(String(signatureText), signature, sizeof(signature), signatureLen)) return false;
    String input = canonicalSigningInput(doc);
    return verifyEs256RawSignature(reinterpret_cast<const uint8_t*>(input.c_str()),
                                   input.length(), signature, signatureLen);
}

bool usesServerChallengeFlow(JsonVariantConst doc) {
    const char* authFlow = doc["auth_flow"] | "";
    const char* challenge = doc["challenge"] | "";
    const char* serverNonce = doc["server_nonce"] | "";
    return strcmp(authFlow, "challenge") == 0 ||
           strcmp(authFlow, "nonce") == 0 ||
           nonEmpty(challenge) ||
           nonEmpty(serverNonce);
}

}  // namespace

void begin() {
    loadReplayCache();
}

Result validate(JsonVariantConst doc, const String& deviceMac) {
    loadReplayCache();
    Result result;

    const char* cmd = doc["cmd"] | "";
    const char* commandId = doc["command_id"] | "";
    const char* issuedAtText = doc["issued_at"] | "";
    const char* expiresAtText = doc["expires_at"] | "";
    const char* nonce = doc["nonce"] | "";
    const char* actor = doc["actor"] | "";
    const char* jwt = doc["jwt"] | "";
    const char* signature = doc["signature"] | "";

    result.commandId = commandId;
    result.nonce = nonce;
    result.actor = actor;

    if (!nonEmpty(cmd) || !nonEmpty(commandId) || !nonEmpty(issuedAtText) ||
        !nonEmpty(expiresAtText) || !nonEmpty(nonce) || !nonEmpty(actor)) {
        result.error = "missing_envelope_field";
        return result;
    }
    if (!nonEmpty(jwt) && !nonEmpty(signature)) {
        result.error = "missing_authenticator";
        return result;
    }
    if (strlen(commandId) >= kReplayValueMax || strlen(nonce) >= kReplayValueMax) {
        result.error = "envelope_field_too_long";
        return result;
    }

    time_t issuedAt = 0;
    time_t expiresAt = 0;
    if (!parseEpoch(doc["issued_at"], issuedAt) || !parseEpoch(doc["expires_at"], expiresAt)) {
        result.error = "invalid_timestamp";
        return result;
    }
    const bool challengeFlow = usesServerChallengeFlow(doc);
    time_t now = ForgeKeyTime::epochNow();
    if (expiresAt <= issuedAt) {
        result.error = "expired";
        return result;
    }
    if (!ForgeKeyTime::clockValid() && !challengeFlow) {
        result.error = "clock_invalid";
        return result;
    }
    if (ForgeKeyTime::clockValid() && now > expiresAt) {
        result.error = "expired";
        return result;
    }
    if (ForgeKeyTime::clockValid() && issuedAt > now + kIssuedAtFutureSkewS) {
        result.error = "issued_in_future";
        return result;
    }
    if (replaySeen(result.commandId, result.nonce)) {
        result.error = "replay_detected";
        return result;
    }

    const char* authError = "invalid_signature";
    bool authenticated = nonEmpty(jwt)
        ? validateJwt(doc, deviceMac, jwt, challengeFlow, authError)
        : validateDetachedSignature(doc, signature);
    if (!authenticated) {
        result.error = authError;
        return result;
    }

    result.ok = true;
    result.error = nullptr;
    return result;
}

void rememberAccepted(const Result& result) {
    if (!result.ok || !result.commandId.length() || !result.nonce.length()) return;
    ReplayEntry& entry = g_replayCache[g_replayNext];
    if (!copyBounded(entry.commandId, sizeof(entry.commandId), result.commandId) ||
        !copyBounded(entry.nonce, sizeof(entry.nonce), result.nonce)) {
        return;
    }
    if (g_replayCount < kReplayCacheSize) g_replayCount++;
    g_replayNext = (g_replayNext + 1) % kReplayCacheSize;
    persistReplayCache();
}

const char* canonicalError(const Result& result) {
    return result.error ? result.error : "invalid_command";
}

}  // namespace CommandValidation
