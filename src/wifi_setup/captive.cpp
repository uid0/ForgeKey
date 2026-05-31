#include "captive.h"

#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiMulti.h>
#include <Preferences.h>
#include <lwip/dns.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <time.h>
#if __has_include(<esp_wpa2.h>)
#include <esp_wpa2.h>
#endif

#include "secrets.h"

namespace WifiSetup {

namespace {
constexpr const char* kPrefsNs = "fk_wifi";
constexpr const char* kProfilesKey = "profiles";
constexpr unsigned long kConnectTimeoutMs = 20000;
RuntimeHealth g_health;
unsigned long g_lastHealthSampleMs = 0;
int32_t g_rssiTotal = 0;
uint16_t g_rssiSamples = 0;
String g_activeLabel;

bool parseIp(const char* text, IPAddress& out) {
    if (!text || !*text) return false;
    return out.fromString(text);
}

String ipToString(const IPAddress& ip) {
    return ip ? ip.toString() : String("");
}

String jsonEscape(const String& value) {
    String out;
    out.reserve(value.length() + 4);
    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];
        if (c == '\"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out += c;
        }
    }
    return out;
}

const char* dnsPolicyName(DnsPolicy policy) {
    switch (policy) {
        case DnsPolicy::Static: return "static";
        case DnsPolicy::FallbackPublic: return "fallback_public";
        case DnsPolicy::Dhcp:
        default: return "dhcp";
    }
}

DnsPolicy parseDnsPolicy(const char* value) {
    if (!value || !*value) return DnsPolicy::Dhcp;
    if (strcmp(value, "static") == 0) return DnsPolicy::Static;
    if (strcmp(value, "fallback_public") == 0) return DnsPolicy::FallbackPublic;
    return DnsPolicy::Dhcp;
}

EnterpriseType parseEnterpriseType(const char* value) {
    if (!value || !*value) return EnterpriseType::None;
    if (strcmp(value, "peap") == 0) return EnterpriseType::Peap;
    if (strcmp(value, "ttls") == 0) return EnterpriseType::Ttls;
    return EnterpriseType::None;
}

const char* enterpriseTypeName(EnterpriseType type) {
    switch (type) {
        case EnterpriseType::Peap: return "peap";
        case EnterpriseType::Ttls: return "ttls";
        case EnterpriseType::None:
        default: return "none";
    }
}

void set_dns_servers(IPAddress dns1, IPAddress dns2) {
    if (!dns1) return;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_dns_info_t dns_info{};
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        dns_info.ip.u_addr.ip4.addr = static_cast<uint32_t>(dns1);
        esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns_info);
        if (dns2) {
            dns_info.ip.u_addr.ip4.addr = static_cast<uint32_t>(dns2);
            esp_netif_set_dns_info(sta, ESP_NETIF_DNS_BACKUP, &dns_info);
        }
    }

    ip_addr_t server{};
    server.type = IPADDR_TYPE_V4;
    server.u_addr.ip4.addr = static_cast<uint32_t>(dns1);
    dns_setserver(0, &server);
    if (dns2) {
        server.u_addr.ip4.addr = static_cast<uint32_t>(dns2);
        dns_setserver(1, &server);
    }
}

void applyDnsPolicy(const WifiProfile& profile) {
    if (profile.dnsPolicy == DnsPolicy::Static) {
        set_dns_servers(profile.dns1 ? profile.dns1 : profile.staticIp.dns1,
                        profile.dns2 ? profile.dns2 : profile.staticIp.dns2);
        Serial.printf("wifi: DNS policy static %s / %s\n",
                      WiFi.dnsIP(0).toString().c_str(), WiFi.dnsIP(1).toString().c_str());
    } else if (profile.dnsPolicy == DnsPolicy::FallbackPublic && !WiFi.dnsIP(0)) {
        set_dns_servers(IPAddress(1, 1, 1, 1), IPAddress(8, 8, 8, 8));
        Serial.println("wifi: DHCP supplied no DNS; fallback_public set to 1.1.1.1 / 8.8.8.8");
    } else {
        Serial.printf("wifi: DNS policy %s using %s / %s\n", dnsPolicyName(profile.dnsPolicy),
                      WiFi.dnsIP(0).toString().c_str(), WiFi.dnsIP(1).toString().c_str());
    }
}

void updateHealthConnected(const WifiProfile* profile = nullptr) {
    g_health.currentRssi = WiFi.RSSI();
    if (g_rssiSamples == 0) {
        g_health.rssiMin = g_health.rssiMax = g_health.currentRssi;
    } else {
        g_health.rssiMin = min(g_health.rssiMin, g_health.currentRssi);
        g_health.rssiMax = max(g_health.rssiMax, g_health.currentRssi);
    }
    g_rssiTotal += g_health.currentRssi;
    g_rssiSamples++;
    g_health.rssiAvg = g_rssiSamples ? (int)(g_rssiTotal / g_rssiSamples) : g_health.currentRssi;
    g_health.bssid = WiFi.BSSIDstr();
    g_health.channel = WiFi.channel();
    g_health.ip = WiFi.localIP();
    g_health.subnet = WiFi.subnetMask();
    g_health.gateway = WiFi.gatewayIP();
    g_health.dns1 = WiFi.dnsIP(0);
    g_health.dns2 = WiFi.dnsIP(1);
    g_health.ssid = WiFi.SSID();
    if (profile) {
        g_activeLabel = profile->label;
        g_health.profileLabel = profile->label;
        g_health.staticIp = profile->staticIp.enabled;
        g_health.dnsPolicy = dnsPolicyName(profile->dnsPolicy);
    } else {
        g_health.profileLabel = g_activeLabel;
    }
}

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        g_health.disconnectReason = info.wifi_sta_disconnected.reason;
        g_health.reconnectCount++;
        Serial.printf("wifi: disconnected reason=%d reconnect_count=%u\n",
                      g_health.disconnectReason, (unsigned)g_health.reconnectCount);
    } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        updateHealthConnected();
    }
}

bool enterpriseSupported(const WifiProfile& profile) {
    if (profile.enterprise.type == EnterpriseType::None) return true;
#if defined(ESP_IDF_VERSION_MAJOR)
    return true;
#else
    (void)profile;
    return false;
#endif
}

void configureEnterprise(const WifiProfile& profile) {
    if (profile.enterprise.type == EnterpriseType::None) return;
#if __has_include(<esp_wpa2.h>)
    esp_wifi_sta_wpa2_ent_clear_identity();
    esp_wifi_sta_wpa2_ent_clear_username();
    esp_wifi_sta_wpa2_ent_clear_password();
    if (profile.enterprise.identity.length()) {
        esp_wifi_sta_wpa2_ent_set_identity((const uint8_t*)profile.enterprise.identity.c_str(), profile.enterprise.identity.length());
    }
    if (profile.enterprise.username.length()) {
        esp_wifi_sta_wpa2_ent_set_username((const uint8_t*)profile.enterprise.username.c_str(), profile.enterprise.username.length());
    }
    if (profile.enterprise.password.length()) {
        esp_wifi_sta_wpa2_ent_set_password((const uint8_t*)profile.enterprise.password.c_str(), profile.enterprise.password.length());
    }
    if (profile.enterprise.caCertPem.length()) {
        esp_wifi_sta_wpa2_ent_set_ca_cert((const uint8_t*)profile.enterprise.caCertPem.c_str(), profile.enterprise.caCertPem.length() + 1);
    }
    esp_wifi_sta_wpa2_ent_enable();
#endif
}

void disconnectForNextAttempt() {
    WiFi.disconnect(false, true);
    delay(200);
}

bool connectProfile(WifiProfile& profile, unsigned long timeoutMs, bool persistSuccess) {
    if (!profile.ssid.length()) return false;
    if (!enterpriseSupported(profile)) {
        Serial.printf("wifi: enterprise profile '%s' unsupported by this build\n", profile.label.c_str());
        return false;
    }
    Serial.printf("wifi: trying profile label='%s' ssid='%s' priority=%d\n",
                  profile.label.c_str(), profile.ssid.c_str(), profile.priority);
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    if (profile.staticIp.enabled) {
        WiFi.config(profile.staticIp.ip, profile.staticIp.gateway, profile.staticIp.subnet,
                    profile.staticIp.dns1, profile.staticIp.dns2);
    } else {
        WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    }
    configureEnterprise(profile);
    WiFi.begin(profile.ssid.c_str(),
               profile.enterprise.type == EnterpriseType::None ? profile.password.c_str() : nullptr);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
        delay(250);
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("wifi: profile '%s' failed status=%d\n", profile.label.c_str(), WiFi.status());
        disconnectForNextAttempt();
        return false;
    }
    applyDnsPolicy(profile);
    updateHealthConnected(&profile);
    if (persistSuccess) {
        profile.lastSuccessEpoch = (uint32_t)time(nullptr);
    }
    Serial.printf("wifi: connected profile='%s' ip=%s\n", profile.label.c_str(), WiFi.localIP().toString().c_str());
    return true;
}

void sortProfiles(WifiProfile* profiles, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = i + 1; j < count; ++j) {
            bool swap = profiles[j].priority < profiles[i].priority ||
                        (profiles[j].priority == profiles[i].priority &&
                         profiles[j].lastSuccessEpoch > profiles[i].lastSuccessEpoch);
            if (swap) {
                WifiProfile tmp = profiles[i];
                profiles[i] = profiles[j];
                profiles[j] = tmp;
            }
        }
    }
}

void profileToJson(const WifiProfile& p, JsonObject obj, bool includeSecret) {
    obj["ssid"] = p.ssid;
    obj["label"] = p.label;
    obj["priority"] = p.priority;
    obj["last_success"] = p.lastSuccessEpoch;
    obj["dns_policy"] = dnsPolicyName(p.dnsPolicy);
    if (includeSecret && p.password.length()) obj["password"] = p.password;
    if (p.dns1) obj["dns1"] = p.dns1.toString();
    if (p.dns2) obj["dns2"] = p.dns2.toString();
    if (p.staticIp.enabled) {
        JsonObject s = obj["static_ip"].to<JsonObject>();
        s["ip"] = p.staticIp.ip.toString();
        s["gateway"] = p.staticIp.gateway.toString();
        s["subnet"] = p.staticIp.subnet.toString();
        if (p.staticIp.dns1) s["dns1"] = p.staticIp.dns1.toString();
        if (p.staticIp.dns2) s["dns2"] = p.staticIp.dns2.toString();
    }
    if (p.enterprise.type != EnterpriseType::None) {
        JsonObject e = obj["enterprise"].to<JsonObject>();
        e["type"] = enterpriseTypeName(p.enterprise.type);
        e["identity"] = p.enterprise.identity;
        e["username"] = p.enterprise.username;
        if (includeSecret) e["password"] = p.enterprise.password;
        if (includeSecret) e["ca_cert"] = p.enterprise.caCertPem;
    }
}

bool jsonToProfile(JsonVariantConst v, WifiProfile& p, const WifiProfile* existing = nullptr) {
    if (existing) p = *existing;
    const char* ssid = v["ssid"] | nullptr;
    if (ssid) p.ssid = ssid;
    const char* pass = v["password"] | nullptr;
    if (pass) p.password = pass;
    const char* label = v["label"] | nullptr;
    if (label) p.label = label;
    if (!p.label.length()) p.label = p.ssid;
    if (v["priority"].is<int>()) p.priority = v["priority"].as<int>();
    if (v["last_success"].is<uint32_t>()) p.lastSuccessEpoch = v["last_success"].as<uint32_t>();
    p.dnsPolicy = parseDnsPolicy(v["dns_policy"] | dnsPolicyName(p.dnsPolicy));
    parseIp(v["dns1"] | "", p.dns1);
    parseIp(v["dns2"] | "", p.dns2);
    if (v["static_ip"].is<JsonObjectConst>()) {
        JsonObjectConst s = v["static_ip"].as<JsonObjectConst>();
        p.staticIp.enabled = s["enabled"] | true;
        parseIp(s["ip"] | "", p.staticIp.ip);
        parseIp(s["gateway"] | "", p.staticIp.gateway);
        parseIp(s["subnet"] | "", p.staticIp.subnet);
        parseIp(s["dns1"] | "", p.staticIp.dns1);
        parseIp(s["dns2"] | "", p.staticIp.dns2);
    }
    if (v["enterprise"].is<JsonObjectConst>()) {
        JsonObjectConst e = v["enterprise"].as<JsonObjectConst>();
        p.enterprise.type = parseEnterpriseType(e["type"] | enterpriseTypeName(p.enterprise.type));
        p.enterprise.identity = e["identity"] | p.enterprise.identity;
        p.enterprise.username = e["username"] | p.enterprise.username;
        p.enterprise.password = e["password"] | p.enterprise.password;
        p.enterprise.caCertPem = e["ca_cert"] | p.enterprise.caCertPem;
    }
    return p.ssid.length() > 0;
}

bool loadProfilesInternal(WifiProfile* profiles, size_t maxProfiles, size_t& count) {
    count = 0;
    Preferences prefs;
    if (prefs.begin(kPrefsNs, true)) {
        String json = prefs.getString(kProfilesKey, "");
        prefs.end();
        if (json.length()) {
            DynamicJsonDocument doc(4096);
            if (!deserializeJson(doc, json)) {
                JsonArrayConst arr = doc["profiles"].as<JsonArrayConst>();
                for (JsonVariantConst item : arr) {
                    if (count >= maxProfiles) break;
                    if (jsonToProfile(item, profiles[count])) count++;
                }
            }
        }
    }
#ifdef FORGEKEY_NETWORK_1_SSID
    if (count < maxProfiles) {
        profiles[count].ssid = FORGEKEY_NETWORK_1_SSID;
        profiles[count].password = FORGEKEY_NETWORK_1_PASSWORD;
        profiles[count].label = "compiled-1";
        profiles[count].priority = 900;
        profiles[count].dnsPolicy = DnsPolicy::Dhcp;
        count++;
    }
#endif
#ifdef FORGEKEY_NETWORK_2_SSID
    if (count < maxProfiles) {
        profiles[count].ssid = FORGEKEY_NETWORK_2_SSID;
        profiles[count].password = FORGEKEY_NETWORK_2_PASSWORD;
        profiles[count].label = "compiled-2";
        profiles[count].priority = 901;
        profiles[count].dnsPolicy = DnsPolicy::Dhcp;
        count++;
    }
#endif
    sortProfiles(profiles, count);
    return count > 0;
}

}  // namespace

void debug_dns_state(const char* label) {
    Serial.printf("[DNS] === %s ===\n", label);
    Serial.printf("[DNS]   Arduino dnsIP(0) = %s\n", WiFi.dnsIP(0).toString().c_str());
    Serial.printf("[DNS]   Arduino dnsIP(1) = %s\n", WiFi.dnsIP(1).toString().c_str());
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_dns_info_t dns_info{};
        esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns_info);
        IPAddress mainDns(dns_info.ip.u_addr.ip4.addr);
        Serial.printf("[DNS]   esp-netif main DNS = %s (raw=0x%08X)\n", mainDns.toString().c_str(), dns_info.ip.u_addr.ip4.addr);
        esp_netif_get_dns_info(sta, ESP_NETIF_DNS_BACKUP, &dns_info);
        IPAddress backupDns(dns_info.ip.u_addr.ip4.addr);
        Serial.printf("[DNS]   esp-netif backup DNS = %s (raw=0x%08X)\n", backupDns.toString().c_str(), dns_info.ip.u_addr.ip4.addr);
    }
    for (int i = 0; i < DNS_MAX_SERVERS; i++) {
        const ip_addr_t* lwip_dns = dns_getserver(i);
        IPAddress srvDns(lwip_dns ? lwip_dns->u_addr.ip4.addr : 0);
        Serial.printf("[DNS]   lwIP dns[%d] = %s\n", i, srvDns.toString().c_str());
    }
    Serial.println("[DNS] === end ===\n");
}

String apSsid() {
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    String last4 = mac.substring(mac.length() - 4);
    last4.toUpperCase();
    return String("ForgeKey-Setup-") + last4;
}

bool loadProfiles(WifiProfile* profiles, size_t maxProfiles, size_t& count) {
    return loadProfilesInternal(profiles, maxProfiles, count);
}

bool saveProfiles(const WifiProfile* profiles, size_t count) {
    DynamicJsonDocument doc(4096);
    JsonArray arr = doc["profiles"].to<JsonArray>();
    for (size_t i = 0; i < count && i < kMaxStoredProfiles; ++i) {
        profileToJson(profiles[i], arr.add<JsonObject>(), true);
    }
    String json;
    serializeJson(doc, json);
    Preferences prefs;
    if (!prefs.begin(kPrefsNs, false)) return false;
    bool ok = prefs.putString(kProfilesKey, json) == json.length();
    prefs.end();
    return ok;
}

bool connectOrPortal(unsigned long portalTimeoutSeconds) {
    static bool eventRegistered = false;
    if (!eventRegistered) {
        WiFi.onEvent(onWifiEvent);
        eventRegistered = true;
    }

    WifiProfile profiles[kMaxStoredProfiles];
    size_t count = 0;
    if (loadProfilesInternal(profiles, kMaxStoredProfiles, count)) {
        Serial.printf("wifi: trying %u stored/compiled profile(s)\n", (unsigned)count);
        for (size_t i = 0; i < count; ++i) {
            if (connectProfile(profiles[i], kConnectTimeoutMs, true)) {
                saveProfiles(profiles, count);
                return true;
            }
        }
    } else {
        Serial.println("wifi: no stored profiles; falling back to captive portal");
    }

    WiFiManager wm;
    wm.setDebugOutput(false);
    wm.setConfigPortalBlocking(true);
    if (portalTimeoutSeconds > 0) wm.setConfigPortalTimeout(portalTimeoutSeconds);
    String ssid = apSsid();
    Serial.printf("wifi: AP SSID '%s' (pw '%s')\n", ssid.c_str(), FORGEKEY_AP_PASSWORD);
    bool connected = wm.autoConnect(ssid.c_str(), FORGEKEY_AP_PASSWORD);
    if (connected && WiFi.status() == WL_CONNECTED) {
        WifiProfile portal;
        portal.ssid = WiFi.SSID();
        portal.label = portal.ssid;
        portal.priority = 500;
        portal.lastSuccessEpoch = (uint32_t)time(nullptr);
        portal.dnsPolicy = DnsPolicy::Dhcp;
        profiles[0] = portal;
        saveProfiles(profiles, 1);
        updateHealthConnected(&portal);
    }
    return connected;
}

bool applyDesiredState(JsonVariantConst desired,
                       ReachabilityProbe reachabilityProbe,
                       String& resultMessage,
                       unsigned long connectTimeoutMs,
                       unsigned long reachabilityTimeoutMs) {
    JsonVariantConst wifi = desired;
    if (!desired["wifi"].isNull()) {
        wifi = desired["wifi"];
    }
    JsonArrayConst incoming = wifi["profiles"].as<JsonArrayConst>();
    if (incoming.isNull()) {
        resultMessage = "missing wifi.profiles";
        return false;
    }

    WifiProfile existing[kMaxStoredProfiles];
    size_t existingCount = 0;
    loadProfilesInternal(existing, kMaxStoredProfiles, existingCount);
    WifiProfile candidate[kMaxStoredProfiles];
    size_t candidateCount = 0;
    for (JsonVariantConst item : incoming) {
        if (candidateCount >= kMaxStoredProfiles) break;
        const char* ssid = item["ssid"] | "";
        const WifiProfile* match = nullptr;
        for (size_t i = 0; i < existingCount; ++i) {
            if (existing[i].ssid == ssid) { match = &existing[i]; break; }
        }
        if (jsonToProfile(item, candidate[candidateCount], match)) candidateCount++;
    }
    if (candidateCount == 0) {
        resultMessage = "no valid wifi profiles";
        return false;
    }
    sortProfiles(candidate, candidateCount);

    for (size_t i = 0; i < candidateCount; ++i) {
        if (!connectProfile(candidate[i], connectTimeoutMs, false)) continue;
        bool reachable = reachabilityProbe ? reachabilityProbe(reachabilityTimeoutMs) : true;
        if (!reachable) {
            Serial.printf("wifi: profile '%s' connected but reachability probe failed\n", candidate[i].label.c_str());
            disconnectForNextAttempt();
            continue;
        }
        candidate[i].lastSuccessEpoch = (uint32_t)time(nullptr);
        if (!saveProfiles(candidate, candidateCount)) {
            resultMessage = "connected but failed to commit profiles";
            return false;
        }
        updateHealthConnected(&candidate[i]);
        resultMessage = String("committed profile ") + candidate[i].label;
        return true;
    }

    resultMessage = "all candidate profiles failed test or reachability";
    // Restore previous known-good connection before returning failure.
    for (size_t i = 0; i < existingCount; ++i) {
        if (connectProfile(existing[i], connectTimeoutMs, false)) break;
    }
    return false;
}

RuntimeHealth health() {
    tickHealth();
    return g_health;
}

void appendHealthJson(String& payload) {
    RuntimeHealth h = health();
    payload += ",\"wifi\":{";
    payload += "\"disconnect_reason\":" + String(h.disconnectReason);
    payload += ",\"reconnect_count\":" + String((unsigned long)h.reconnectCount);
    payload += ",\"rssi\":" + String(h.currentRssi);
    payload += ",\"rssi_min\":" + String(h.rssiMin);
    payload += ",\"rssi_max\":" + String(h.rssiMax);
    payload += ",\"rssi_avg\":" + String(h.rssiAvg);
    payload += ",\"bssid\":\"" + jsonEscape(h.bssid) + "\"";
    payload += ",\"channel\":" + String(h.channel);
    payload += ",\"ssid\":\"" + jsonEscape(h.ssid) + "\"";
    payload += ",\"profile_label\":\"" + jsonEscape(h.profileLabel) + "\"";
    payload += ",\"static_ip\":" + String(h.staticIp ? "true" : "false");
    payload += ",\"dns_policy\":\"" + String(h.dnsPolicy) + "\"";
    payload += ",\"lease\":{";
    payload += "\"ip\":\"" + ipToString(h.ip) + "\"";
    payload += ",\"subnet\":\"" + ipToString(h.subnet) + "\"";
    payload += ",\"gateway\":\"" + ipToString(h.gateway) + "\"";
    payload += ",\"dns1\":\"" + ipToString(h.dns1) + "\"";
    payload += ",\"dns2\":\"" + ipToString(h.dns2) + "\"";
    payload += "}}";
}

void tickHealth() {
    if (WiFi.status() != WL_CONNECTED) return;
    unsigned long now = millis();
    if (g_lastHealthSampleMs != 0 && now - g_lastHealthSampleMs < 30000) return;
    g_lastHealthSampleMs = now;
    updateHealthConnected();
}

void forgetAndRestart() {
    Serial.println("wifi: forgetting credentials/profiles, restarting into portal");
    Preferences prefs;
    if (prefs.begin(kPrefsNs, false)) {
        prefs.clear();
        prefs.end();
    }
    WiFiManager wm;
    wm.resetSettings();
    delay(200);
    ESP.restart();
}

}  // namespace WifiSetup
