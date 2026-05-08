#include "captive.h"

#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiMulti.h>
#include <lwip/dns.h>
#include <esp_netif.h>

#include "secrets.h"

namespace WifiSetup {

void debug_dns_state(const char* label) {
    Serial.printf("[DNS] === %s ===\n", label);

    // Check Arduino-level DNS
    IPAddress dns0 = WiFi.dnsIP(0);
    IPAddress dns1 = WiFi.dnsIP(1);
    Serial.printf("[DNS]   Arduino dnsIP(0) = %s\n", dns0.toString().c_str());
    Serial.printf("[DNS]   Arduino dnsIP(1) = %s\n", dns1.toString().c_str());

    // Check esp-netif DNS
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_dns_info_t dns_info{};
        esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns_info);
        IPAddress mainDns;
        mainDns[0] = dns_info.ip.u_addr.ip4.addr & 0xFF;
        mainDns[1] = (dns_info.ip.u_addr.ip4.addr >> 8) & 0xFF;
        mainDns[2] = (dns_info.ip.u_addr.ip4.addr >> 16) & 0xFF;
        mainDns[3] = (dns_info.ip.u_addr.ip4.addr >> 24) & 0xFF;
        Serial.printf("[DNS]   esp-netif main DNS = %s (raw=0x%08X)\n",
                     mainDns.toString().c_str(), dns_info.ip.u_addr.ip4.addr);

        esp_netif_get_dns_info(sta, ESP_NETIF_DNS_BACKUP, &dns_info);
        IPAddress backupDns;
        backupDns[0] = dns_info.ip.u_addr.ip4.addr & 0xFF;
        backupDns[1] = (dns_info.ip.u_addr.ip4.addr >> 8) & 0xFF;
        backupDns[2] = (dns_info.ip.u_addr.ip4.addr >> 16) & 0xFF;
        backupDns[3] = (dns_info.ip.u_addr.ip4.addr >> 24) & 0xFF;
        Serial.printf("[DNS]   esp-netif backup DNS = %s (raw=0x%08X)\n",
                     backupDns.toString().c_str(), dns_info.ip.u_addr.ip4.addr);
    } else {
        Serial.println("[DNS]   esp-netif STA handle is NULL");
    }

    // Check lwIP DNS server table
    for (int i = 0; i < DNS_MAX_SERVERS; i++) {
        const ip_addr_t* lwip_dns = dns_getserver(i);
        if (!lwip_dns) {
            Serial.printf("[DNS]   lwIP dns[%d] = (null)\n", i);
            continue;
        }
        IPAddress srvDns;
        srvDns[0] = lwip_dns->u_addr.ip4.addr & 0xFF;
        srvDns[1] = (lwip_dns->u_addr.ip4.addr >> 8) & 0xFF;
        srvDns[2] = (lwip_dns->u_addr.ip4.addr >> 16) & 0xFF;
        srvDns[3] = (lwip_dns->u_addr.ip4.addr >> 24) & 0xFF;
        Serial.printf("[DNS]   lwIP dns[%d] = %s (raw=0x%08X type=%d)\n",
                     i, srvDns.toString().c_str(),
                     lwip_dns->u_addr.ip4.addr, lwip_dns->type);
    }

    // Try to resolve a known host
    IPAddress testIp;
    bool resolved = WiFi.hostByName("pool.ntp.org", testIp);
    Serial.printf("[DNS]   hostByName(pool.ntp.org) = %s -> %s\n",
                 resolved ? "OK" : "FAILED", testIp.toString().c_str());
    Serial.println("[DNS] === end ===\n");
}

namespace {

void set_dns_servers(IPAddress dns1, IPAddress dns2) {
    Serial.printf("[DNS] Setting DNS: primary=%s secondary=%s\n",
                 dns1.toString().c_str(),
                 dns2.toString().c_str());

    // Method 1: esp-netif DNS config
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_dns_info_t dns_info{};
        dns_info.ip.u_addr.ip4.addr = static_cast<uint32_t>(dns1);
        esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns_info);
        Serial.printf("[DNS]   esp-netif set MAIN DNS = 0x%08X\n", dns_info.ip.u_addr.ip4.addr);

        if (dns2) {
            dns_info.ip.u_addr.ip4.addr = static_cast<uint32_t>(dns2);
            esp_netif_set_dns_info(sta, ESP_NETIF_DNS_BACKUP, &dns_info);
            Serial.printf("[DNS]   esp-netif set BACKUP DNS = 0x%08X\n", dns_info.ip.u_addr.ip4.addr);
        }
    } else {
        Serial.println("[DNS]   WARNING: esp-netif STA handle is NULL");
    }

    // Method 2: lwIP DNS server table
    ip_addr_t dns_servers[DNS_MAX_SERVERS];
    memset(dns_servers, 0, sizeof(dns_servers));

    dns_servers[0].type = IPADDR_TYPE_V4;
    dns_servers[0].u_addr.ip4.addr = static_cast<uint32_t>(dns1);
    dns_setserver(0, &dns_servers[0]);
    Serial.printf("[DNS]   lwIP set dns[0] = 0x%08X type=%d\n",
                 dns_servers[0].u_addr.ip4.addr, dns_servers[0].type);

    if (dns2) {
        dns_servers[1].type = IPADDR_TYPE_V4;
        dns_servers[1].u_addr.ip4.addr = static_cast<uint32_t>(dns2);
        dns_setserver(1, &dns_servers[1]);
        Serial.printf("[DNS]   lwIP set dns[1] = 0x%08X type=%d\n",
                     dns_servers[1].u_addr.ip4.addr, dns_servers[1].type);
    }

    // Verify what we just set
    debug_dns_state("after set_dns_servers");
}

#if defined(FORGEKEY_NETWORK_1_SSID) || defined(FORGEKEY_NETWORK_2_SSID)
#define FORGEKEY_HAS_PREDEFINED_NETWORKS 1
#else
#define FORGEKEY_HAS_PREDEFINED_NETWORKS 0
#endif

#if FORGEKEY_HAS_PREDEFINED_NETWORKS
// Per-attempt timeout for WiFiMulti.run(). Long enough for a slow AP scan
// + DHCP, short enough that a missing dev network doesn't noticeably
// delay portal fallback.
constexpr unsigned long kPredefinedTimeoutMs = 15000;

bool tryPredefinedNetworks() {
    WiFiMulti multi;
#ifdef FORGEKEY_NETWORK_1_SSID
    multi.addAP(FORGEKEY_NETWORK_1_SSID, FORGEKEY_NETWORK_1_PASSWORD);
#endif
#ifdef FORGEKEY_NETWORK_2_SSID
    multi.addAP(FORGEKEY_NETWORK_2_SSID, FORGEKEY_NETWORK_2_PASSWORD);
#endif
    Serial.println("wifi: trying predefined networks...");
    WiFi.mode(WIFI_STA);
    if (multi.run(kPredefinedTimeoutMs) == WL_CONNECTED) {
        Serial.printf("wifi: connected to '%s' via predefined list\n",
                      WiFi.SSID().c_str());
        IPAddress dns1(8, 8, 8, 8);
        IPAddress dns2(1, 1, 1, 1);
        set_dns_servers(dns1, dns2);
        Serial.println("wifi: DNS set to 8.8.8.8 / 1.1.1.1");
        return true;
    }
    Serial.println("wifi: predefined networks unavailable, falling back");
    return false;
}
#endif  // FORGEKEY_HAS_PREDEFINED_NETWORKS

}  // namespace

String apSsid() {
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    String last4 = mac.substring(mac.length() - 4);
    last4.toUpperCase();
    return String("ForgeKey-Setup-") + last4;
}

bool connectOrPortal(unsigned long portalTimeoutSeconds) {
#if FORGEKEY_HAS_PREDEFINED_NETWORKS
    if (tryPredefinedNetworks()) {
        return true;
    }
#endif

    WiFiManager wm;
    wm.setDebugOutput(false);
    wm.setConfigPortalBlocking(true);
    if (portalTimeoutSeconds > 0) {
        wm.setConfigPortalTimeout(portalTimeoutSeconds);
    }

    String ssid = apSsid();
    Serial.printf("wifi: AP SSID '%s' (pw '%s')\n",
                  ssid.c_str(), FORGEKEY_AP_PASSWORD);

    // autoConnect tries the saved creds first; if missing or failing it
    // raises the AP + DNS captive portal and blocks until the user submits
    // creds (or the optional timeout fires).
    bool connected = wm.autoConnect(ssid.c_str(), FORGEKEY_AP_PASSWORD);
    if (connected && WiFi.status() == WL_CONNECTED) {
        IPAddress dns1(8, 8, 8, 8);
        IPAddress dns2(1, 1, 1, 1);
        set_dns_servers(dns1, dns2);
        Serial.println("wifi: DNS set to 8.8.8.8 / 1.1.1.1");
    }
    return connected;
}

void forgetAndRestart() {
    Serial.println("wifi: forgetting credentials, restarting into portal");
    WiFiManager wm;
    wm.resetSettings();
    delay(200);
    ESP.restart();
}

}  // namespace WifiSetup
