#include "local_status_web.h"

#if !defined(FORGEKEY_EPAPER) && !defined(FORGEKEY_DISABLE_LOCAL_STATUS_WEB)

#include <WebServer.h>
#include <WiFi.h>

namespace LocalStatusWeb {
namespace {
WebServer g_server(80);
StatusProvider g_provider = nullptr;
String g_mac;
bool g_started = false;

String htmlEscape(const String& value) {
    String out;
    out.reserve(value.length() + 16);
    for (size_t i = 0; i < value.length(); ++i) {
        char ch = value[i];
        if (ch == '&') out += F("&amp;");
        else if (ch == '<') out += F("&lt;");
        else if (ch == '>') out += F("&gt;");
        else if (ch == '"') out += F("&quot;");
        else out += ch;
    }
    return out;
}

String statusJson() {
    if (g_provider) return g_provider();
    return String(F("{\"error\":\"status_provider_unavailable\"}"));
}

void handleRoot() {
    const String json = statusJson();
    String body;
    body.reserve(json.length() + 700);
    body += F("<!doctype html><html><head><meta charset='utf-8'>"
              "<meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<meta http-equiv='refresh' content='15'>"
              "<title>ForgeKey Status</title>"
              "<style>body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;margin:2rem;line-height:1.4}"
              "code,pre{background:#f3f4f6;border-radius:.5rem;padding:.75rem;overflow:auto}"
              ".ok{color:#047857}.warn{color:#b45309}</style></head><body>");
    body += F("<h1>ForgeKey local status</h1><p><b>MAC:</b> <code>");
    body += htmlEscape(g_mac);
    body += F("</code></p><p><b>IP:</b> <code>");
    body += WiFi.localIP().toString();
    body += F("</code></p><p class='warn'>Read-only support page. Disable on constrained/battery-only classes with <code>FORGEKEY_DISABLE_LOCAL_STATUS_WEB</code>.</p>");
    body += F("<p><a href='/status.json'>Open JSON status</a></p><h2>Status JSON</h2><pre>");
    body += htmlEscape(json);
    body += F("</pre></body></html>");
    g_server.send(200, F("text/html; charset=utf-8"), body);
}

void handleStatusJson() {
    g_server.sendHeader(F("Cache-Control"), F("no-store"));
    g_server.send(200, F("application/json"), statusJson());
}
}  // namespace

void begin(const String& mac, StatusProvider provider) {
    if (g_started || WiFi.status() != WL_CONNECTED) return;
    g_mac = mac;
    g_provider = provider;
    g_server.on(F("/"), HTTP_GET, handleRoot);
    g_server.on(F("/status.json"), HTTP_GET, handleStatusJson);
    g_server.begin();
    g_started = true;
    Serial.printf("[LOCAL_STATUS_WEB] started http://%s/\n", WiFi.localIP().toString().c_str());
}

void tick() {
    if (g_started) g_server.handleClient();
}

bool active() { return g_started; }

}  // namespace LocalStatusWeb

#else

namespace LocalStatusWeb {
void begin(const String&, StatusProvider) {}
void tick() {}
bool active() { return false; }
}  // namespace LocalStatusWeb

#endif
