#include "register.h"
#include "device_config.h"

#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_csr.h>

#include "security/oms_ca.h"
#include "security/oms_command_pubkey.h"
#include "../capabilities/status_led/status_led.h"

Provisioning provisioning;

namespace {
constexpr const char* kNvsNamespace = "forgekey";
constexpr const char* kKeyDeviceId = "dev_id";
constexpr const char* kKeyFwTopic  = "fw_topic";
constexpr const char* kKeyPingTopic = "p_topic";
constexpr const char* kKeyClientCert = "c_cert";
constexpr const char* kKeyClientKey  = "c_key";
constexpr const char* kKeyCmdPubKey  = "cmd_pub";
constexpr const char* kKeyBoots    = "boots";
constexpr const char* kKeyProvTok  = "prov_tok";  // OTA-delivered rotation override
constexpr const char* kKeyBrokerHost = "b_host";
constexpr const char* kKeyBrokerPort = "b_port";
constexpr const char* kKeyBrokerTls  = "b_tls";
constexpr const char* kPlaceholderToken = "REPLACE_ME_PROVISIONING_TOKEN";

// Multipart boundary used for the multipart/form-data enrollment POST.
constexpr const char* kBoundary = "----ForgekeyBoundary7d3f2c1a";

const char* chipModelName(esp_chip_model_t model) {
    switch (model) {
        case CHIP_ESP32:
            return "ESP32";
        case CHIP_ESP32S2:
            return "ESP32-S2";
        case CHIP_ESP32S3:
            return "ESP32-S3";
        case CHIP_ESP32C3:
            return "ESP32-C3";
        case CHIP_ESP32H2:
            return "ESP32-H2";
        default:
            return "Unknown";
    }
}

void addChipFeatures(JsonObject features, uint32_t chipFeatures) {
    features["embedded_flash"] = (chipFeatures & CHIP_FEATURE_EMB_FLASH) != 0;
    features["wifi_bgn"] = (chipFeatures & CHIP_FEATURE_WIFI_BGN) != 0;
    features["ble"] = (chipFeatures & CHIP_FEATURE_BLE) != 0;
    features["bt"] = (chipFeatures & CHIP_FEATURE_BT) != 0;
    features["ieee802154"] = (chipFeatures & CHIP_FEATURE_IEEE802154) != 0;
    features["embedded_psram"] = (chipFeatures & CHIP_FEATURE_EMB_PSRAM) != 0;
}

bool generateDeviceKeyAndCsr(const String& mac,
                             String& privateKeyPem,
                             String& csrPem) {
    constexpr const char* kPersonalization = "forgekey-enroll";
    unsigned char privateKeyBuf[2048];
    unsigned char csrBuf[2048];
    char subjectName[64];
    snprintf(subjectName, sizeof(subjectName), "CN=forgekey-%s", mac.c_str());

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    mbedtls_entropy_context entropy;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_context ctrDrbg;
    mbedtls_ctr_drbg_init(&ctrDrbg);
    mbedtls_x509write_csr req;
    mbedtls_x509write_csr_init(&req);

    int rc = mbedtls_ctr_drbg_seed(&ctrDrbg, mbedtls_entropy_func, &entropy,
                                   reinterpret_cast<const unsigned char*>(kPersonalization),
                                   strlen(kPersonalization));
    if (rc != 0) {
        Serial.printf("enroll: ctr_drbg_seed failed: -0x%04x\n", -rc);
        goto fail;
    }

    rc = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (rc != 0) {
        Serial.printf("enroll: pk_setup failed: -0x%04x\n", -rc);
        goto fail;
    }

    rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk),
                             mbedtls_ctr_drbg_random, &ctrDrbg);
    if (rc != 0) {
        Serial.printf("enroll: ecp_gen_key failed: -0x%04x\n", -rc);
        goto fail;
    }

    rc = mbedtls_pk_write_key_pem(&pk, privateKeyBuf, sizeof(privateKeyBuf));
    if (rc != 0) {
        Serial.printf("enroll: write_key_pem failed: -0x%04x\n", -rc);
        goto fail;
    }
    privateKeyPem = reinterpret_cast<const char*>(privateKeyBuf);

    mbedtls_x509write_csr_set_md_alg(&req, MBEDTLS_MD_SHA256);
    mbedtls_x509write_csr_set_key(&req, &pk);
    rc = mbedtls_x509write_csr_set_subject_name(&req, subjectName);
    if (rc != 0) {
        Serial.printf("enroll: set_subject_name failed: -0x%04x\n", -rc);
        goto fail;
    }

    rc = mbedtls_x509write_csr_pem(&req, csrBuf, sizeof(csrBuf),
                                   mbedtls_ctr_drbg_random, &ctrDrbg);
    if (rc != 0) {
        Serial.printf("enroll: write_csr_pem failed: -0x%04x\n", -rc);
        goto fail;
    }
    csrPem = reinterpret_cast<const char*>(csrBuf);

    mbedtls_x509write_csr_free(&req);
    mbedtls_ctr_drbg_free(&ctrDrbg);
    mbedtls_entropy_free(&entropy);
    mbedtls_pk_free(&pk);
    return true;

fail:
    mbedtls_x509write_csr_free(&req);
    mbedtls_ctr_drbg_free(&ctrDrbg);
    mbedtls_entropy_free(&entropy);
    mbedtls_pk_free(&pk);
    return false;
}

JsonVariantConst responsePolicy(const DynamicJsonDocument& resp) {
    JsonVariantConst policy = resp["policy"];
    if (policy.isNull()) {
        return resp.as<JsonVariantConst>();
    }
    return policy;
}

String firstString(JsonVariantConst primary, const char* fallback = "") {
    if (!primary.isNull()) {
        const char* value = primary.as<const char*>();
        if (value && *value) return String(value);
    }
    return String(fallback ? fallback : "");
}

String firstString(JsonVariantConst primary,
                   JsonVariantConst secondary,
                   const char* fallback = "") {
    if (!primary.isNull()) {
        const char* value = primary.as<const char*>();
        if (value && *value) return String(value);
    }
    if (!secondary.isNull()) {
        const char* value = secondary.as<const char*>();
        if (value && *value) return String(value);
    }
    return String(fallback ? fallback : "");
}

uint16_t firstU16(JsonVariantConst primary, JsonVariantConst secondary, uint16_t fallback = 0) {
    if (!primary.isNull()) return static_cast<uint16_t>(primary.as<unsigned int>());
    if (!secondary.isNull()) return static_cast<uint16_t>(secondary.as<unsigned int>());
    return fallback;
}

bool firstBool(JsonVariantConst primary, JsonVariantConst secondary, bool fallback = false) {
    if (!primary.isNull()) return primary.as<bool>();
    if (!secondary.isNull()) return secondary.as<bool>();
    return fallback;
}
}  // namespace

void Provisioning::begin() {
    Preferences p;
    p.begin(kNvsNamespace, /*readOnly=*/false);
    cachedBootCount = p.getUInt(kKeyBoots, 0) + 1;
    p.putUInt(kKeyBoots, cachedBootCount);
    p.end();

    load();
}

void Provisioning::load() {
    Preferences p;
    p.begin(kNvsNamespace, /*readOnly=*/true);
    creds.deviceId          = p.getString(kKeyDeviceId, "");
    creds.mqttFirmwareTopic = p.getString(kKeyFwTopic, "");
    creds.mqttPingsTopic    = p.getString(kKeyPingTopic, "");
    creds.clientCertificatePem = p.getString(kKeyClientCert, "");
    creds.clientPrivateKeyPem  = p.getString(kKeyClientKey, "");
    creds.commandPublicKeyPem  = p.getString(kKeyCmdPubKey, "");
    creds.mqttBrokerHost    = p.getString(kKeyBrokerHost, "");
    creds.mqttBrokerPort    = (uint16_t)p.getUShort(kKeyBrokerPort, 0);
    creds.mqttBrokerUseTls  = p.getBool(kKeyBrokerTls, false);
    p.end();

    if (creds.commandPublicKeyPem.length() == 0) {
        creds.commandPublicKeyPem = String(kOmsCommandPubKeyPem);
    }

    // Verbose: dump exactly what we loaded so a wrong/empty topic in NVS
    // is visible at boot rather than silently falling back to defaults.
    Serial.printf("provisioning: loaded from NVS device_id='%s' (len=%u)\n",
                  creds.deviceId.c_str(), (unsigned)creds.deviceId.length());
    Serial.printf("provisioning: loaded from NVS mqtt_topic_for_pings='%s' (len=%u)\n",
                  creds.mqttPingsTopic.c_str(), (unsigned)creds.mqttPingsTopic.length());
    Serial.printf("provisioning: loaded from NVS mqtt_topic_for_firmware='%s' (len=%u)\n",
                  creds.mqttFirmwareTopic.c_str(), (unsigned)creds.mqttFirmwareTopic.length());
    Serial.printf("provisioning: loaded from NVS client_cert len=%u client_key len=%u cmd_pub len=%u\n",
                  (unsigned)creds.clientCertificatePem.length(),
                  (unsigned)creds.clientPrivateKeyPem.length(),
                  (unsigned)creds.commandPublicKeyPem.length());
    Serial.printf("provisioning: loaded from NVS mqtt_broker host='%s' port=%u use_tls=%d\n",
                  creds.mqttBrokerHost.c_str(), (unsigned)creds.mqttBrokerPort,
                  (int)creds.mqttBrokerUseTls);
}

bool Provisioning::isProvisioned() const {
    return creds.deviceId.length() > 0 &&
           creds.clientCertificatePem.length() > 0 &&
           creds.clientPrivateKeyPem.length() > 0;
}

void Provisioning::clear() {
    Preferences p;
    p.begin(kNvsNamespace, /*readOnly=*/false);
    p.remove(kKeyDeviceId);
    p.remove(kKeyFwTopic);
    p.remove(kKeyPingTopic);
    p.remove(kKeyClientCert);
    p.remove(kKeyClientKey);
    p.remove(kKeyCmdPubKey);
    p.remove(kKeyBrokerHost);
    p.remove(kKeyBrokerPort);
    p.remove(kKeyBrokerTls);
    p.end();
    creds = DeviceCredentials{};
}

DeviceCredentials Provisioning::credentials() const { return creds; }

String Provisioning::activeProvisioningToken() const {
    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/true)) {
        return String(FORGEKEY_PROVISIONING_TOKEN);
    }
    String stored = p.getString(kKeyProvTok, "");
    p.end();
    return stored.length() > 0 ? stored : String(FORGEKEY_PROVISIONING_TOKEN);
}

bool Provisioning::setProvisioningToken(const String& token) {
    if (token.length() == 0) return false;
    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/false)) return false;
    size_t written = p.putString(kKeyProvTok, token);
    p.end();
    return written > 0;
}

bool Provisioning::persist(const DeviceCredentials& c) {
    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/false)) return false;
    p.putString(kKeyDeviceId, c.deviceId);
    p.putString(kKeyFwTopic, c.mqttFirmwareTopic);
    p.putString(kKeyPingTopic, c.mqttPingsTopic);
    p.putString(kKeyClientCert, c.clientCertificatePem);
    p.putString(kKeyClientKey, c.clientPrivateKeyPem);
    p.putString(kKeyCmdPubKey, c.commandPublicKeyPem);
    p.putString(kKeyBrokerHost, c.mqttBrokerHost);
    p.putUShort(kKeyBrokerPort, c.mqttBrokerPort);
    p.putBool(kKeyBrokerTls, c.mqttBrokerUseTls);
    p.end();
    creds = c;
    return true;
}

bool Provisioning::enrollDevice(const char* host, uint16_t port,
                                const String& mac,
                                const String& ipAddr,
                                const uint8_t* jpegBuf, size_t jpegLen) {
    // jpegBuf == nullptr / jpegLen == 0 is allowed for non-imaging sensor
    // kinds (e.g. temperature_sensor). The multipart body in that case
    // contains only the metadata part.
    const bool hasPhoto = (jpegBuf != nullptr && jpegLen > 0);
    String privateKeyPem;
    String csrPem;
    if (!generateDeviceKeyAndCsr(mac, privateKeyPem, csrPem)) {
        Serial.println("enroll: failed to generate device keypair/CSR");
        return false;
    }

    // Build the JSON metadata part once so we can compute Content-Length.
    DynamicJsonDocument meta(4096);
    esp_chip_info_t chipInfo{};
    esp_chip_info(&chipInfo);

    char uniqueChipId[17];
    snprintf(uniqueChipId, sizeof(uniqueChipId), "%016llx",
             (unsigned long long)ESP.getEfuseMac());

    uint32_t flashMemoryId = 0;
    esp_err_t flashIdErr = esp_flash_read_id(nullptr, &flashMemoryId);

    meta["mac_address"]      = mac;
    meta["firmware_version"] = FORGEKEY_FIRMWARE_VERSION;
    meta["sensor_kind"]      = FORGEKEY_SENSOR_KIND;
    meta["boot_count"]       = cachedBootCount;
    meta["free_heap"]        = ESP.getFreeHeap();
    meta["ip"]               = ipAddr;
    meta["csr_pem"]          = csrPem;
    meta["unique_chip_id"]   = uniqueChipId;
    if (flashIdErr == ESP_OK) {
        char flashMemoryIdHex[11];
        snprintf(flashMemoryIdHex, sizeof(flashMemoryIdHex), "0x%06lx",
                 (unsigned long)flashMemoryId);
        meta["flash_memory_id"] = flashMemoryIdHex;
    } else {
        meta["flash_memory_id"] = nullptr;
        Serial.printf("enroll: failed to read flash memory id: %s\n",
                      esp_err_to_name(flashIdErr));
    }

    JsonObject chipInfoJson = meta.createNestedObject("chip_info");
    chipInfoJson["model"] = chipModelName(chipInfo.model);
    chipInfoJson["cores"] = chipInfo.cores;
    chipInfoJson["revision"] = chipInfo.revision;
    chipInfoJson["full_revision"] = chipInfo.full_revision;
    addChipFeatures(chipInfoJson.createNestedObject("features"), chipInfo.features);

    Serial.printf("enroll: chip_info model=%s cores=%u revision=%u full_revision=%u "
                  "unique_chip_id=%s flash_memory_id=%s csr_len=%u\n",
                  chipInfoJson["model"].as<const char*>(),
                  (unsigned)chipInfo.cores,
                  (unsigned)chipInfo.revision,
                  (unsigned)chipInfo.full_revision,
                  uniqueChipId,
                  flashIdErr == ESP_OK ? meta["flash_memory_id"].as<const char*>() : "(unavailable)",
                  (unsigned)csrPem.length());
    String metaJson;
    serializeJson(meta, metaJson);

    String head;
    head.reserve(1024);
    head += "--"; head += kBoundary; head += "\r\n";
    head += "Content-Disposition: form-data; name=\"metadata\"\r\n";
    head += "Content-Type: application/json\r\n\r\n";
    head += metaJson;
    if (hasPhoto) {
        head += "\r\n--"; head += kBoundary; head += "\r\n";
        head += "Content-Disposition: form-data; name=\"photo\"; filename=\"boot.jpg\"\r\n";
        head += "Content-Type: image/jpeg\r\n\r\n";
    }

    String tail;
    tail.reserve(64);
    tail += "\r\n--"; tail += kBoundary; tail += "--\r\n";

    size_t totalLen = head.length() + (hasPhoto ? jpegLen : 0) + tail.length();
    String activeToken = activeProvisioningToken();

    String tokenPreview = activeToken.substring(0, 8);
    Serial.printf("enroll: POST host=%s port=%u path=/api/forgekey/devices/enroll/ "
                  "url=https://%s:%u/api/forgekey/devices/enroll/ "
                  "body_bytes=%u token_prefix=%s... token_len=%u\n",
                  host, port, host, port, (unsigned)totalLen,
                  tokenPreview.c_str(), (unsigned)activeToken.length());
    if (activeToken == kPlaceholderToken) {
        Serial.println("enroll: WARNING using placeholder provisioning token; OMS will reject this with 401");
    }

    WiFiClientSecure tls;
    tls.setCACert(kOmsCaPem);
    tls.setTimeout(15);

    // DNS resolver needs time to initialize after DHCP. Wait first, then retry.
    delay(2000);
    IPAddress resolvedIp;
    for (int retry = 0; retry < 5; retry++) {
        if (WiFi.hostByName(host, resolvedIp)) {
            break;
        }
        delay(1000);
    }
    if (!WiFi.hostByName(host, resolvedIp)) {
        Serial.printf("enroll: DNS failed for %s after retries\n", host);
        return false;
    }

    if (!tls.connect(host, port)) {
        Serial.printf("enroll: TLS connect to %s:%u failed\n", host, port);
        return false;
    }

    tls.printf("POST /api/forgekey/devices/enroll/ HTTP/1.1\r\n"
               "Host: %s\r\n"
               "X-ForgeKey-Provisioning-Token: %s\r\n"
               "Content-Type: multipart/form-data; boundary=%s\r\n"
               "Content-Length: %u\r\n"
               "Connection: close\r\n\r\n",
               host, activeToken.c_str(), kBoundary,
               (unsigned)totalLen);
    tls.print(head);
    if (hasPhoto) {
        // Stream the JPEG in chunks so we don't double-allocate the buffer.
        const size_t kChunk = 1024;
        for (size_t off = 0; off < jpegLen; off += kChunk) {
            size_t n = (jpegLen - off < kChunk) ? (jpegLen - off) : kChunk;
            if (tls.write(jpegBuf + off, n) != n) {
                Serial.println("enroll: TLS write failed mid-photo");
                tls.stop();
                return false;
            }
        }
    }
    tls.print(tail);

    // Read status line.
    String statusLine = tls.readStringUntil('\n');
    statusLine.trim();
    int sp1 = statusLine.indexOf(' ');
    int sp2 = (sp1 >= 0) ? statusLine.indexOf(' ', sp1 + 1) : -1;
    int code = (sp1 >= 0 && sp2 > sp1)
                   ? statusLine.substring(sp1 + 1, sp2).toInt()
                   : 0;

    // Skip headers up to blank line.
    while (tls.connected() || tls.available()) {
        String line = tls.readStringUntil('\n');
        if (line.length() <= 1) break;  // "\r" only
    }

    // Read the body (server is expected to keep it small).
    String body;
    body.reserve(512);
    while (tls.available() || tls.connected()) {
        while (tls.available()) body += (char)tls.read();
        if (!tls.available()) delay(10);
        if (body.length() > 4096) break;  // hard cap, shouldn't happen
    }
    tls.stop();

    Serial.printf("enroll: HTTP %d (%s) body_len=%u\n",
                  code, statusLine.c_str(), (unsigned)body.length());

    if (code < 200 || code >= 300) {
        return false;
    }

    DynamicJsonDocument resp(12288);
    DeserializationError err = deserializeJson(resp, body);
    if (err) {
        Serial.printf("enroll: JSON parse error: %s\n", err.c_str());
        return false;
    }

    DeviceCredentials c;
    JsonVariantConst policy = responsePolicy(resp);
    c.deviceId             = firstString(resp["device_id"]);
    c.clientCertificatePem = firstString(resp["client_certificate_pem"],
                                         resp["certificate_pem"]);
    c.clientPrivateKeyPem  = privateKeyPem;
    c.commandPublicKeyPem  = firstString(resp["command_public_key_pem"],
                                         resp["oms_command_public_key_pem"],
                                         kOmsCommandPubKeyPem);
    c.mqttFirmwareTopic    = firstString(policy["mqtt_topic_for_firmware"],
                                         resp["mqtt_topic_for_firmware"]);
    c.mqttPingsTopic       = firstString(policy["mqtt_topic_for_pings"],
                                         resp["mqtt_topic_for_pings"]);
    c.mqttBrokerHost       = firstString(policy["mqtt_broker_host"],
                                         resp["mqtt_broker_host"]);
    c.mqttBrokerPort       = firstU16(policy["mqtt_broker_port"],
                                      resp["mqtt_broker_port"], 0);
    c.mqttBrokerUseTls     = firstBool(policy["mqtt_broker_use_tls"],
                                       resp["mqtt_broker_use_tls"], false);

    Serial.printf("enroll: parsed device_id='%s'\n", c.deviceId.c_str());
    Serial.printf("enroll: parsed mqtt_topic_for_pings='%s'\n",
                  c.mqttPingsTopic.c_str());
    Serial.printf("enroll: parsed mqtt_topic_for_firmware='%s'\n",
                  c.mqttFirmwareTopic.c_str());
    Serial.printf("enroll: parsed client_cert len=%u client_key len=%u cmd_pub len=%u\n",
                  (unsigned)c.clientCertificatePem.length(),
                  (unsigned)c.clientPrivateKeyPem.length(),
                  (unsigned)c.commandPublicKeyPem.length());
    Serial.printf("enroll: parsed mqtt_broker_host='%s' mqtt_broker_port=%u mqtt_broker_use_tls=%d\n",
                  c.mqttBrokerHost.c_str(), (unsigned)c.mqttBrokerPort,
                  (int)c.mqttBrokerUseTls);

    // Warn if broker config is missing from the enrollment response.
    // Without these fields the device falls back to compile-time defaults,
    // which may point to the wrong broker or use the wrong port/TLS setting.
    if (c.mqttBrokerHost.length() == 0 || c.mqttBrokerPort == 0) {
        Serial.println("enroll: WARNING — enrollment response missing "
                       "mqtt_broker_host/port/use_tls fields. "
                       "Device will use compile-time fallback defaults.");
    }

    if (c.deviceId.length() == 0 ||
        c.clientCertificatePem.length() == 0 ||
        c.clientPrivateKeyPem.length() == 0) {
        Serial.println("enroll: response missing device_id or client certificate bundle");
        return false;
    }

    if (!persist(c)) {
        Serial.println("enroll: NVS persist failed");
        return false;
    }
    // Dump every key written to NVS so we can confirm the bytes that future
    // boots will read back (matches the "loaded from NVS" lines in load()).
    Serial.printf("enroll: NVS persisted device_id='%s' (len=%u)\n",
                  c.deviceId.c_str(), (unsigned)c.deviceId.length());
    Serial.printf("enroll: NVS persisted mqtt_topic_for_pings='%s' (len=%u)\n",
                  c.mqttPingsTopic.c_str(), (unsigned)c.mqttPingsTopic.length());
    Serial.printf("enroll: NVS persisted mqtt_topic_for_firmware='%s' (len=%u)\n",
                  c.mqttFirmwareTopic.c_str(), (unsigned)c.mqttFirmwareTopic.length());
    Serial.printf("enroll: NVS persisted client_cert len=%u client_key len=%u cmd_pub len=%u\n",
                  (unsigned)c.clientCertificatePem.length(),
                  (unsigned)c.clientPrivateKeyPem.length(),
                  (unsigned)c.commandPublicKeyPem.length());
    Serial.printf("enroll: NVS persisted mqtt_broker host='%s' port=%u use_tls=%d\n",
                   c.mqttBrokerHost.c_str(), (unsigned)c.mqttBrokerPort,
                   (int)c.mqttBrokerUseTls);
    Serial.printf("enroll: provisioned as %s\n", c.deviceId.c_str());

    // Flash LED to indicate HTTP POST completed
    extern void triggerMessageFlash();
    StatusLed::triggerMessageFlash();

    return true;
}
