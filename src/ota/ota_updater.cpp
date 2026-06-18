#include "ota_updater.h"

#include <ArduinoJson.h>
#include <Update.h>
#include "watchdog/watchdog_manager.h"
#include <WiFiClientSecure.h>
#include <WiFiClient.h>
#include <WiFi.h>
#include <mbedtls/sha256.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#if __has_include(<esp_app_desc.h>)
#include <esp_app_desc.h>
#endif
#include <Preferences.h>
#include <time.h>
#if __has_include(<esp_efuse.h>)
#include <esp_efuse.h>
#endif

#include "security/oms_ca.h"
#include "security/firmware_verify.h"
#include "provisioning/device_config.h"
#include "provisioning/register.h"
#include "capabilities/registry.h"
#include "build/build_metadata.h"

#ifndef ARDUINO_BOARD
#define ARDUINO_BOARD "arduino"
#endif

OtaUpdater otaUpdater;

void OtaUpdater::begin() {
    // Nothing to set up at boot. markStableIfPending() handles validation
    // once the rest of the system has proven itself.
}

void OtaUpdater::notify(const char* state, const char* version, int progress, const char* error) {
    if (statusCb) statusCb(state, version, progress, error);
}

namespace {

String firstString(JsonVariantConst primary, JsonVariantConst fallback, const char* key) {
    if (!primary[key].isNull()) {
        if (primary[key].is<const char*>()) return String(primary[key].as<const char*>());
        if (primary[key].is<unsigned long>()) return String(primary[key].as<unsigned long>());
    }
    if (!fallback[key].isNull()) {
        if (fallback[key].is<const char*>()) return String(fallback[key].as<const char*>());
        if (fallback[key].is<unsigned long>()) return String(fallback[key].as<unsigned long>());
    }
    return String("");
}

bool firstBool(JsonVariantConst primary, JsonVariantConst fallback, const char* key, bool def) {
    if (!primary[key].isNull()) return primary[key].as<bool>();
    if (!fallback[key].isNull()) return fallback[key].as<bool>();
    return def;
}

uint32_t parseDeadlineEpoch(const String& deadline) {
    if (deadline.length() == 0) return 0;
    bool digitsOnly = true;
    for (size_t i = 0; i < deadline.length(); ++i) {
        if (!isDigit(deadline[i])) { digitsOnly = false; break; }
    }
    if (digitsOnly) return (uint32_t)deadline.toInt();
    // ISO-8601 parsing is intentionally lightweight: Arduino newlib accepts
    // neither timegm nor strptime consistently across ESP32 cores. The raw
    // deadline string is still surfaced to telemetry; OMS should send epoch_s
    // when device-side enforcement needs wall-clock certainty.
    return 0;
}

int compareVersion(const String& a, const String& b) {
    int ia = 0;
    int ib = 0;
    while (ia < (int)a.length() || ib < (int)b.length()) {
        while (ia < (int)a.length() && !isDigit(a[ia])) ia++;
        while (ib < (int)b.length() && !isDigit(b[ib])) ib++;
        unsigned long va = 0;
        unsigned long vb = 0;
        while (ia < (int)a.length() && isDigit(a[ia])) { va = va * 10 + (a[ia++] - '0'); }
        while (ib < (int)b.length() && isDigit(b[ib])) { vb = vb * 10 + (b[ib++] - '0'); }
        if (va < vb) return -1;
        if (va > vb) return 1;
        if (ia >= (int)a.length() && ib >= (int)b.length()) return 0;
    }
    return 0;
}

uint8_t deviceCohortBucket() {
    String mac = WiFi.macAddress();
    uint32_t hash = 2166136261UL;
    for (size_t i = 0; i < mac.length(); ++i) {
        char ch = mac[i];
        if (ch == ':') continue;
        hash ^= (uint8_t)tolower(ch);
        hash *= 16777619UL;
    }
    return (uint8_t)(hash % 100);
}

bool tokenListContains(const String& csv, const String& value) {
    int start = 0;
    while (start <= (int)csv.length()) {
        int comma = csv.indexOf(',', start);
        String token = (comma < 0) ? csv.substring(start) : csv.substring(start, comma);
        token.trim();
        if (token == value) return true;
        if (comma < 0) break;
        start = comma + 1;
    }
    return false;
}

bool matchesCapabilityTarget(const String& target) {
    if (target.length() == 0 || target == "all" || target == "*") return true;
    if (target == FORGEKEY_SENSOR_KIND) return true;
    for (Capability* c = CapabilityRegistry::head(); c; c = c->next) {
        if (c->active && target == c->id) return true;
    }
    return false;
}

bool matchesRolloutCohort(const String& cohort) {
    if (cohort.length() == 0 || cohort == "all" || cohort == "*") return true;
    uint8_t bucket = deviceCohortBucket();
    char bucketName[5];
    snprintf(bucketName, sizeof(bucketName), "c%02u", bucket);
    if (cohort == bucketName) return true;
    if (cohort.startsWith("pct:")) {
        int pct = cohort.substring(4).toInt();
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        return bucket < pct;
    }
    return tokenListContains(cohort, String(bucketName));
}

void jsonStringField(String& payload, const char* key, const String& value) {
    payload += ",\"";
    payload += key;
    payload += "\":\"";
    for (size_t i = 0; i < value.length(); ++i) {
        char ch = value[i];
        if (ch == '\\' || ch == '"') payload += '\\';
        payload += ch;
    }
    payload += "\"";
}
const char* otaStateName(esp_ota_img_states_t state) {
    switch (state) {
        case ESP_OTA_IMG_NEW: return "new";
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
        case ESP_OTA_IMG_VALID: return "valid";
        case ESP_OTA_IMG_INVALID: return "invalid";
        case ESP_OTA_IMG_ABORTED: return "aborted";
        case ESP_OTA_IMG_UNDEFINED: default: return "undefined";
    }
}

}  // namespace

bool OtaUpdater::parse(const uint8_t* payload, unsigned int length, Spec& out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
        Serial.printf("ota: parse error: %s\n", err.c_str());
        return false;
    }
    JsonVariantConst policy = doc["policy"];
    if (policy.isNull()) policy = doc.as<JsonVariantConst>();

    out.url         = firstString(doc, policy, "url");
    out.sha256      = firstString(doc, policy, "sha256");
    out.signature   = firstString(doc, policy, "signature");
    out.signingCert = firstString(doc, policy, "signing_cert");
    out.version     = firstString(doc, policy, "version");
    out.mandatory   = firstBool(doc, policy, "mandatory", false);
    out.minimumVersion = firstString(policy, doc, "minimum_version");
    out.maximumVersion = firstString(policy, doc, "maximum_version");
    out.hardwareTarget = firstString(policy, doc, "hardware_target");
    out.capabilityTarget = firstString(policy, doc, "capability_target");
    out.rolloutCohort = firstString(policy, doc, "rollout_cohort");
    out.deadline = firstString(policy, doc, "deadline");
    if (out.deadline.length() == 0) {
        if (!policy["deadline_epoch"].isNull()) {
            out.deadline = String(policy["deadline_epoch"].as<unsigned long>());
        } else if (!doc["deadline_epoch"].isNull()) {
            out.deadline = String(doc["deadline_epoch"].as<unsigned long>());
        }
    }
    out.deadlineEpoch = parseDeadlineEpoch(out.deadline);

    if (out.url.length() == 0 || out.sha256.length() != 64) {
        Serial.println("ota: missing url or invalid sha256");
        return false;
    }
    if (out.signature.length() == 0) {
        Serial.println("ota: dispatch missing signature — refusing");
        return false;
    }
    out.sha256.toLowerCase();
    return true;
}

bool OtaUpdater::isPolicyAllowed(const Spec& spec, String& reason) const {
    if (spec.minimumVersion.length() &&
        compareVersion(String(FORGEKEY_FIRMWARE_VERSION), spec.minimumVersion) < 0) {
        reason = "below_minimum_version";
        return false;
    }
    if (spec.maximumVersion.length() &&
        compareVersion(String(FORGEKEY_FIRMWARE_VERSION), spec.maximumVersion) > 0) {
        reason = "above_maximum_version";
        return false;
    }
    if (spec.hardwareTarget.length() && spec.hardwareTarget != "all" &&
        spec.hardwareTarget != "*" && spec.hardwareTarget != FORGEKEY_BUILD_TARGET) {
        reason = "hardware_target_mismatch";
        return false;
    }
    if (!matchesCapabilityTarget(spec.capabilityTarget)) {
        reason = "capability_target_mismatch";
        return false;
    }
    if (!matchesRolloutCohort(spec.rolloutCohort)) {
        reason = "rollout_cohort_mismatch";
        return false;
    }
    if (spec.deadlineEpoch != 0) {
        time_t now = time(nullptr);
        if (now > 1600000000 && now >= (time_t)spec.deadlineEpoch) {
            // Deadlines are an urgency signal, not a rejection criterion.
            Serial.println("ota: deadline has passed; treating policy as urgent");
        }
    }
    reason = "";
    return true;
}

bool OtaUpdater::hexEq(const String& a, const String& b) {
    if (a.length() != b.length()) return false;
    for (size_t i = 0; i < a.length(); i++) {
        char ca = a[i]; if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        char cb = b[i]; if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

namespace {

// Parse a URL of the form https://host[:port]/path or http://host[:port]/path.
bool splitUrl(const String& url, bool& tls, String& host, uint16_t& port, String& path) {
    int schemeEnd = url.indexOf("://");
    if (schemeEnd < 0) return false;
    String scheme = url.substring(0, schemeEnd);
    scheme.toLowerCase();
    if (scheme == "https") { tls = true;  port = 443; }
    else if (scheme == "http") { tls = false; port = 80; }
    else return false;

    int hostStart = schemeEnd + 3;
    int pathStart = url.indexOf('/', hostStart);
    String hostPort = (pathStart < 0) ? url.substring(hostStart)
                                      : url.substring(hostStart, pathStart);
    int colon = hostPort.indexOf(':');
    if (colon >= 0) {
        host = hostPort.substring(0, colon);
        port = (uint16_t)hostPort.substring(colon + 1).toInt();
    } else {
        host = hostPort;
    }
    path = (pathStart < 0) ? "/" : url.substring(pathStart);
    return host.length() > 0;
}

const char* kHexDigits = "0123456789abcdef";

void toHex(const uint8_t* in, size_t n, char* outHex) {
    for (size_t i = 0; i < n; i++) {
        outHex[i * 2]     = kHexDigits[(in[i] >> 4) & 0xF];
        outHex[i * 2 + 1] = kHexDigits[in[i] & 0xF];
    }
    outHex[n * 2] = '\0';
}

}  // namespace

bool OtaUpdater::apply(const Spec& spec) {
    bool tls = false;
    String host, path;
    uint16_t port = 0;
    if (!splitUrl(spec.url, tls, host, port, path)) {
        Serial.println("ota: malformed URL");
        notify("failed", spec.version.c_str(), -1, "malformed_url");
        return false;
    }

    updating = true;
    notify("downloading", spec.version.c_str(), 0, nullptr);

    WiFiClient* client = nullptr;
    WiFiClientSecure secureClient;
    WiFiClient plainClient;
    if (tls) {
        secureClient.setCACert(kOmsCaPem);
        secureClient.setTimeout(20);
        // Present the device's CA-issued client cert + key when available, so
        // the OMS mTLS firmware-download listener (oms PR #667) accepts us.
        // Harmless when OMS is still serving the legacy token-auth listener:
        // that endpoint doesn't request a client cert, so the leaf is never
        // sent. Provisioning may not yet have completed on a freshly-flashed
        // device — skip in that case and rely on the token / JWT path the
        // dispatcher's URL already carries.
        const DeviceCredentials& creds = provisioning.credentials();
        if (creds.clientCertificatePem.length() > 0 &&
            creds.clientPrivateKeyPem.length() > 0) {
            secureClient.setCertificate(creds.clientCertificatePem.c_str());
            secureClient.setPrivateKey(creds.clientPrivateKeyPem.c_str());
            Serial.printf("ota: presenting mTLS client cert (%u bytes)\n",
                          (unsigned)creds.clientCertificatePem.length());
        }
        client = &secureClient;
    } else {
        client = &plainClient;
    }

    if (!client->connect(host.c_str(), port)) {
        Serial.printf("ota: connect %s:%u failed\n", host.c_str(), port);
        updating = false;
        notify("failed", spec.version.c_str(), -1, "connect_failed");
        return false;
    }

    // Issue the request. Many origins require Host + close + UA.
    client->printf("GET %s HTTP/1.1\r\n"
                   "Host: %s\r\n"
                   "User-Agent: ForgeKey-OTA/1\r\n"
                   "Accept: */*\r\n"
                   "Connection: close\r\n\r\n",
                   path.c_str(), host.c_str());

    // Status line.
    String statusLine = client->readStringUntil('\n');
    statusLine.trim();
    int sp1 = statusLine.indexOf(' ');
    int sp2 = (sp1 >= 0) ? statusLine.indexOf(' ', sp1 + 1) : -1;
    int code = (sp1 >= 0 && sp2 > sp1)
                   ? statusLine.substring(sp1 + 1, sp2).toInt()
                   : 0;
    if (code != 200) {
        Serial.printf("ota: HTTP %d (%s)\n", code, statusLine.c_str());
        client->stop();
        updating = false;
        char err[32];
        snprintf(err, sizeof(err), "http_%d", code);
        notify("failed", spec.version.c_str(), -1, err);
        return false;
    }

    // Headers.
    size_t contentLength = 0;
    while (true) {
        String line = client->readStringUntil('\n');
        line.trim();
        if (line.length() == 0) break;
        if (line.startsWith("Content-Length:") || line.startsWith("content-length:")) {
            contentLength = (size_t)line.substring(line.indexOf(':') + 1).toInt();
        }
    }

    if (contentLength == 0) {
        Serial.println("ota: server did not advertise Content-Length");
        client->stop();
        updating = false;
        notify("failed", spec.version.c_str(), -1, "no_content_length");
        return false;
    }
    Serial.printf("ota: downloading %u bytes for %s\n",
                  (unsigned)contentLength, spec.version.c_str());

    if (!Update.begin(contentLength)) {
        Serial.printf("ota: Update.begin failed: %s\n", Update.errorString());
        client->stop();
        updating = false;
        notify("failed", spec.version.c_str(), -1, "update_begin_failed");
        return false;
    }

    ForgeKeyWatchdog::CriticalSection watchdogSafeFlash("ota_flash_write");

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, /*is224=*/0);

    uint8_t buf[1024];
    size_t received = 0;
    int lastProgressPct = 0;
    unsigned long lastDataMs = millis();
    while (received < contentLength) {
        size_t avail = client->available();
        if (avail == 0) {
            if (!client->connected()) {
                Serial.println("ota: connection closed mid-download");
                Update.abort();
                mbedtls_sha256_free(&sha);
                client->stop();
                updating = false;
                notify("failed", spec.version.c_str(), -1, "connection_closed");
                return false;
            }
            if (millis() - lastDataMs > 20000) {
                Serial.println("ota: read timeout");
                Update.abort();
                mbedtls_sha256_free(&sha);
                client->stop();
                updating = false;
                notify("failed", spec.version.c_str(), -1, "read_timeout");
                return false;
            }
            ForgeKeyWatchdog::markHealthy(ForgeKeyWatchdog::Subsystem::OTA);
            delay(5);
            continue;
        }
        size_t want = sizeof(buf);
        if (want > avail) want = avail;
        if (received + want > contentLength) want = contentLength - received;
        int n = client->read(buf, want);
        if (n <= 0) { delay(2); continue; }
        if ((size_t)Update.write(buf, n) != (size_t)n) {
            Serial.printf("ota: write failed: %s\n", Update.errorString());
            Update.abort();
            mbedtls_sha256_free(&sha);
            client->stop();
            updating = false;
            notify("failed", spec.version.c_str(), -1, "flash_write_failed");
            return false;
        }
        mbedtls_sha256_update(&sha, buf, n);
        received += n;
        lastDataMs = millis();
        ForgeKeyWatchdog::markHealthy(ForgeKeyWatchdog::Subsystem::OTA);

        // Emit a status ping every ~10% of progress. Cheap (one MQTT publish)
        // and lets OMS show a meaningful progress bar without flooding the bus.
        int pct = (int)((received * 100ULL) / contentLength);
        if (pct >= lastProgressPct + 10 && pct < 100) {
            lastProgressPct = pct;
            notify("downloading", spec.version.c_str(), pct, nullptr);
        }
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    client->stop();

    notify("verifying", spec.version.c_str(), 100, nullptr);

    {
        String reason;
        if (!isPolicyAllowed(spec, reason)) {
            Serial.printf("ota: policy rejected during final verification: %s\n", reason.c_str());
            Update.abort();
            updating = false;
            notify("rejected", spec.version.c_str(), -1, reason.c_str());
            return false;
        }
    }

    char actualHex[65];
    toHex(digest, 32, actualHex);
    if (!hexEq(String(actualHex), spec.sha256)) {
        Serial.printf("ota: checksum mismatch — got %s expected %s\n",
                      actualHex, spec.sha256.c_str());
        Update.abort();
        updating = false;
        notify("failed", spec.version.c_str(), -1, "sha256_mismatch");
        return false;
    }

    // ECDSA signature must verify before we hand the partition to the bootloader.
    // A mismatch here means either the binary was tampered with in transit (and
    // collided on SHA-256, which is implausible) or the dispatcher used a key the
    // device doesn't trust — either way we refuse the swap.
    //
    // When the dispatch payload carries `signing_cert`, OMS has rotated to a
    // CA-issued leaf signer (see oms PR #666): verify the cert chains to the
    // burned-in internal CA + carries CODE_SIGNING EKU, then verify the
    // binary signature under the *leaf's* pubkey. When it doesn't,
    // fall back to the embedded firmware pubkey path — both rolling-out
    // devices and OMS deployments without a CA stay supported.
    {
        uint8_t sigBuf[128];  // ECDSA(P-256) DER is ≤ 72 bytes; 128 is comfortable headroom
        size_t sigLen = sizeof(sigBuf);
        if (!firmware_verify::decodeBase64(spec.signature, sigBuf, &sigLen)) {
            Serial.println("ota: signature is not valid base64 — abort");
            Update.abort();
            updating = false;
            notify("failed", spec.version.c_str(), -1, "bad_base64_signature");
            return false;
        }
        bool sigOk;
        if (spec.signingCert.length() > 0) {
            Serial.printf("ota: chained verify (leaf cert %u bytes)\n",
                          (unsigned)spec.signingCert.length());
            sigOk = firmware_verify::verifySignatureChained(
                digest, 32, sigBuf, sigLen, spec.signingCert.c_str());
        } else {
            sigOk = firmware_verify::verifySignature(digest, 32, sigBuf, sigLen);
        }
        if (!sigOk) {
            Serial.println("ota: signature verification failed — abort");
            Update.abort();
            updating = false;
            notify("failed", spec.version.c_str(), -1, "signature_invalid");
            return false;
        }
        Serial.println("ota: signature verified");
    }

    if (!Update.end(/*evenIfRemaining=*/true)) {
        Serial.printf("ota: Update.end failed: %s\n", Update.errorString());
        updating = false;
        notify("failed", spec.version.c_str(), -1, "update_end_failed");
        return false;
    }

    {
        Preferences prefs;
        prefs.begin("ota", false);
        prefs.putString("prev_ver", FORGEKEY_FIRMWARE_VERSION);
        prefs.putString("target_ver", spec.version);
        prefs.end();
    }

    // Tell OMS we're rebooting into the new image. The post-reboot
    // markStableIfPending() call publishes the "applied" event once the new
    // firmware proves it can talk to the broker.
    notify("rebooting", spec.version.c_str(), 100, nullptr);
    Serial.printf("ota: %s installed, rebooting\n", spec.version.c_str());
    delay(250);
    ESP.restart();
    // unreachable
    return true;
}

void OtaUpdater::markStableIfPending() {
    if (stableMarked) return;
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) return;
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            Serial.println("ota: marked running partition as valid");
            // The very first publish post-OTA is the success signal back to
            // OMS. We don't know the prior "downloading" version string here,
            // but the running firmware version is what matters to the operator.
            notify("applied", FORGEKEY_FIRMWARE_VERSION, 100, nullptr);
        } else {
            Serial.printf("ota: mark valid failed: 0x%x\n", err);
        }
    }
    stableMarked = true;
}
void OtaUpdater::appendHealthJson(String& payload) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot = esp_ota_get_boot_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);

    String partition = running ? String(running->label) : String("");
    String bootPartition = boot ? String(boot->label) : String("");
    String nextPartition = next ? String(next->label) : String("");
    ForgeKeyBuildMetadata::appendJson(payload);
    jsonStringField(payload, "ota_partition", partition);
    jsonStringField(payload, "ota_running_slot", partition);
    jsonStringField(payload, "ota_boot_slot", bootPartition);
    jsonStringField(payload, "ota_next_slot", nextPartition);

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    bool pending = false;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK) {
        pending = (state == ESP_OTA_IMG_PENDING_VERIFY);
    }
    jsonStringField(payload, "ota_state", otaStateName(state));
    payload += ",\"ota_pending_verify\":";
    payload += pending ? "true" : "false";

    Preferences prefs;
    prefs.begin("ota", true);
    String previousVersion = prefs.getString("prev_ver", "");
    prefs.end();
    jsonStringField(payload, "ota_previous_version", previousVersion);

    const esp_app_desc_t* app = esp_ota_get_app_description();
    payload += ",\"ota_secure_version\":";
    payload += String((unsigned long)(app ? app->secure_version : 0));
#if __has_include(<esp_efuse.h>)
    bool allowed = app ? esp_efuse_check_secure_version(app->secure_version) : true;
    payload += ",\"ota_anti_rollback_supported\":true";
    payload += ",\"ota_anti_rollback_ok\":";
    payload += allowed ? "true" : "false";
#else
    payload += ",\"ota_anti_rollback_supported\":false";
#endif
}
