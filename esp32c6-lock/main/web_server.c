/*
 * Embedded web server implementation for ESP32-C6 lock device.
 * esp_http_server serving status page and JSON API.
 */

#include "web_server.h"
#include "lock_state.h"
#include "lock_config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_system.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"

static const char* TAG = "WEB";

/* Cached status page HTML */
static char s_html_cache[4096] = {0};
static size_t s_html_len = 0;

/* Cached JSON status */
static char s_json_cache[512] = {0};
static size_t s_json_len = 0;

/* HTTP server handle */
static httpd_handle_t s_server = NULL;

/* OMS deep-link URL components */
static char s_oms_host[64] = {0};
static char s_asset_id[64] = {0};

/* Cached deep-link URL */
static char s_deep_link_url[256] = {0};

/* ===== HTML status page ===== */

static const char* STATUS_PAGE_TEMPLATE =
    "<!DOCTYPE html>"
    "<html lang=\"en\">"
    "<head><meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
    "<title>ForgeKey Cabinet Lock</title>"
    "<style>"
    "* { margin: 0; padding: 0; box-sizing: border-box; }"
    "body {"
    "  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;"
    "  background: #1a1a2e; color: #eee; min-height: 100vh;"
    "  display: flex; flex-direction: column; align-items: center; justify-content: center;"
    "}"
    ".container { text-align: center; padding: 2rem; max-width: 480px; width: 100%; }"
    ".logo { font-size: 2rem; font-weight: 700; margin-bottom: 0.5rem; color: #fff; }"
    ".subtitle { font-size: 0.9rem; color: #888; margin-bottom: 2rem; }"
    ".status-card { background: #16213e; border-radius: 12px; padding: 2rem; margin-bottom: 2rem; border: 2px solid #333; }"
    ".status-label { font-size: 0.85rem; text-transform: uppercase; letter-spacing: 0.1em; color: #888; margin-bottom: 0.5rem; }"
    ".status-value { font-size: 2.5rem; font-weight: 700; }"
    ".status-SECURE { color: #4ade80; border-color: #4ade80; }"
    ".status-UNLOCKED { color: #60a5fa; border-color: #60a5fa; }"
    ".status-ACCESSING { color: #fbbf24; border-color: #fbbf24; }"
    ".status-ALARM { color: #f87171; border-color: #f87171; animation: pulse 0.5s infinite; }"
    ".status-INITIALIZING { color: #a78bfa; border-color: #a78bfa; }"
    "@keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.5; } }"
    ".details { margin-top: 1rem; font-size: 0.85rem; color: #666; text-align: left; }"
    ".details div { display: flex; justify-content: space-between; padding: 0.25rem 0; border-bottom: 1px solid #222; }"
    ".details div:last-child { border-bottom: none; }"
    ".cta-card { background: #16213e; border-radius: 12px; padding: 1.5rem; margin-bottom: 1.5rem; }"
    ".cta-card p { margin-bottom: 1rem; color: #aaa; font-size: 0.95rem; }"
    ".cta-button { display: inline-block; background: #3b82f6; color: #fff; text-decoration: none; padding: 0.75rem 2rem; border-radius: 8px; font-weight: 600; font-size: 1rem; transition: background 0.2s; }"
    ".cta-button:hover { background: #2563eb; }"
    ".footer { font-size: 0.75rem; color: #555; margin-top: 2rem; }"
    "</style>"
    "</head><body>"
    "<div class=\"container\">"
    "  <div class=\"logo\">ForgeKey</div>"
    "  <div class=\"subtitle\">Cabinet Lock</div>"
    "  <div class=\"status-card\" id=\"statusCard\">"
    "    <div class=\"status-label\">Lock Status</div>"
    "    <div class=\"status-value\" id=\"statusValue\">--</div>"
    "    <div class=\"details\">"
    "      <div><span>MAC</span><span id=\"macAddr\">--</span></div>"
    "      <div><span>Reed Switch</span><span id=\"reedStatus\">--</span></div>"
    "      <div><span>Latch Supervisor</span><span id=\"latchStatus\">--</span></div>"
    "      <div><span>IR Beam</span><span id=\"irStatus\">--</span></div>"
    "      <div><span>Mortise Switch</span><span id=\"mortiseStatus\">--</span></div>"
    "      <div><span>Uptime</span><span id=\"uptime\">--</span></div>"
    "    </div>"
    "  </div>"
    "  <div class=\"cta-card\">"
    "    <p>Need to unlock this cabinet? Generate an unlock code from the OpenMakerSuite dashboard.</p>"
    "    <a href=\"__DEEP_LINK__\" target=\"_blank\" class=\"cta-button\">Go to OpenMakerSuite</a>"
    "  </div>"
    "  <div class=\"footer\">ForgeKey Cabinet Lock - Built with ESP32-C6</div>"
    "</div>"
    "<script>"
    "function updateStatus() {"
    "  fetch('/api/status')"
    "    .then(r => r.json())"
    "    .then(data => {"
    "      document.getElementById('statusValue').textContent = data.state || '--';"
    "      document.getElementById('statusValue').className = 'status-value status-' + (data.state || '');"
    "      document.getElementById('statusCard').className = 'status-card status-' + (data.state || '');"
    "      document.getElementById('macAddr').textContent = data.mac || '--';"
    "      document.getElementById('reedStatus').textContent = data.reed_closed ? 'Closed' : 'Open';"
    "      document.getElementById('latchStatus').textContent = data.latch_locked ? 'Locked' : 'Unlocked';"
    "      document.getElementById('irStatus').textContent = data.ir_broken ? 'Broken' : 'Intact';"
    "      document.getElementById('mortiseStatus').textContent = data.mortise_active ? 'Active' : 'Inactive';"
    "      var ms = data.uptime || 0;"
    "      var h = Math.floor(ms / 3600000);"
    "      var m = Math.floor((ms % 3600000) / 60000);"
    "      var s = Math.floor((ms % 60000) / 1000);"
    "      document.getElementById('uptime').textContent = h + 'h ' + m + 'm ' + s + 's';"
    "    })"
    "    .catch(err => { console.error('Failed to fetch status:', err); });"
    "}"
    "updateStatus();"
    "setInterval(updateStatus, 5000);"
    "</script>"
    "</body></html>";

/* ===== HTTP handlers ===== */

static esp_err_t root_get_handler(httpd_req_t* req) {
    /* Build HTML with deep-link URL substituted */
    const char* template = STATUS_PAGE_TEMPLATE;
    const char* placeholder = "__DEEP_LINK__";
    size_t tmpl_len = strlen(template);
    size_t url_len = strlen(s_deep_link_url);
    size_t placeholder_len = strlen(placeholder);

    /* Allocate buffer: template + (url_len - placeholder_len) + margin */
    size_t buf_size = tmpl_len + url_len - placeholder_len + 32;
    char* html_buf = malloc(buf_size);
    if (!html_buf) {
        /* Fallback: send raw template */
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, template, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* Copy template up to placeholder */
    const char* p = strstr(template, placeholder);
    if (p) {
        size_t prefix_len = p - template;
        memcpy(html_buf, template, prefix_len);
        memcpy(html_buf + prefix_len, s_deep_link_url, url_len);
        memcpy(html_buf + prefix_len + url_len, p + placeholder_len, tmpl_len - (p - template) - placeholder_len);
        html_buf[prefix_len + url_len + (tmpl_len - (p - template) - placeholder_len)] = '\0';
    } else {
        /* Placeholder not found — just copy template */
        memcpy(html_buf, template, tmpl_len);
        html_buf[tmpl_len] = '\0';
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_buf, strlen(html_buf));
    free(html_buf);
    return ESP_OK;
}

static esp_err_t api_status_handler(httpd_req_t* req) {
    const char* json = lock_web_server_get_json_status();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static const httpd_uri_t root_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler,
    .user_ctx = NULL
};

static const httpd_uri_t api_status_uri = {
    .uri = "/api/status",
    .method = HTTP_GET,
    .handler = api_status_handler,
    .user_ctx = NULL
};

/* ===== Public API ===== */

void lock_web_server_init(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = FORGEKEY_LOCK_WEB_PORT;
    config.ctrl_port = HTTPD_CTRL_INVALID;
    config.max_uri_handlers = 8;

    if (httpd_start(&s_server, &config) == ESP_OK) {
        httpd_register_uri_handler(s_server, &root_uri);
        httpd_register_uri_handler(s_server, &api_status_uri);
        ESP_LOGI(TAG, "Web server started on port %d", FORGEKEY_LOCK_WEB_PORT);
    } else {
        ESP_LOGE(TAG, "Failed to start web server");
    }
}

void lock_web_server_deinit(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}

void lock_web_server_tick(void) {
    /* esp_http_server handles connections internally via its own task.
     * No explicit tick needed. */
}

const char* lock_web_server_get_html(void) {
    /* Return the template directly */
    return STATUS_PAGE_TEMPLATE;
}

const char* lock_web_server_get_json_status(void) {
    lock_telemetry_t tel = lock_state_get_telemetry();
    lock_state_t state = lock_state_get_state();
    const char* state_name = lock_state_state_name(state);

    s_json_len = snprintf(s_json_cache, sizeof(s_json_cache),
        "{\"mac\":\"%s\",\"secure\":%s,\"item_present\":%s,"
        "\"uptime\":%lu,\"state\":\"%s\",\"reed_closed\":%s,"
        "\"latch_locked\":%s,\"ir_broken\":%s,\"mortise_active\":%s,"
        "\"asset_id\":\"%s\"}",
        lock_state_get_mac_address(),
        tel.secure ? "true" : "false",
        !tel.ir_broken ? "true" : "false",
        (unsigned long)tel.uptime_ms,
        state_name,
        tel.reed_closed ? "true" : "false",
        tel.latch_locked ? "true" : "false",
        tel.ir_broken ? "true" : "false",
        tel.mortise_active ? "true" : "false",
        s_asset_id);

    return s_json_cache;
}

void lock_web_server_set_oms_link(const char* oms_host, const char* asset_id) {
    if (oms_host) {
        strncpy(s_oms_host, oms_host, sizeof(s_oms_host) - 1);
        s_oms_host[sizeof(s_oms_host) - 1] = '\0';
    }
    if (asset_id && asset_id[0] != '\0') {
        strncpy(s_asset_id, asset_id, sizeof(s_asset_id) - 1);
        s_asset_id[sizeof(s_asset_id) - 1] = '\0';
        snprintf(s_deep_link_url, sizeof(s_deep_link_url),
                 "https://%s/fks/%s", s_oms_host, s_asset_id);
    } else {
        s_asset_id[0] = '\0';
        snprintf(s_deep_link_url, sizeof(s_deep_link_url),
                 "https://%s", s_oms_host);
    }
}
