#include "embedded_web.h"
#include "lock_capability.h"
#include "lock_device.h"
#include "lock_config.h"
#include <WebServer.h>
#include <WiFi.h>
#include <ArduinoJson.h>

namespace LockWeb {

static WebServer* s_server = nullptr;
static bool s_started = false;

static const char* HTML_PAGE = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>ForgeKey Cabinet Lock</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body {
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
    background: #1a1a2e;
    color: #eee;
    min-height: 100vh;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
  }
  .container {
    text-align: center;
    padding: 2rem;
    max-width: 480px;
    width: 100%;
  }
  .logo {
    font-size: 2rem;
    font-weight: 700;
    margin-bottom: 0.5rem;
    color: #fff;
  }
  .subtitle {
    font-size: 0.9rem;
    color: #888;
    margin-bottom: 2rem;
  }
  .status-card {
    background: #16213e;
    border-radius: 12px;
    padding: 2rem;
    margin-bottom: 2rem;
    border: 2px solid #333;
  }
  .status-label {
    font-size: 0.85rem;
    text-transform: uppercase;
    letter-spacing: 0.1em;
    color: #888;
    margin-bottom: 0.5rem;
  }
  .status-value {
    font-size: 2.5rem;
    font-weight: 700;
  }
  .status-SECURE { color: #4ade80; border-color: #4ade80; }
  .status-UNLOCKED { color: #60a5fa; border-color: #60a5fa; }
  .status-ACCESSING { color: #fbbf24; border-color: #fbbf24; }
  .status-ALARM { color: #f87171; border-color: #f87171; animation: pulse 0.5s infinite; }
  .status-INITIALIZING { color: #a78bfa; border-color: #a78bfa; }
  @keyframes pulse {
    0%, 100% { opacity: 1; }
    50% { opacity: 0.5; }
  }
  .details {
    margin-top: 1rem;
    font-size: 0.85rem;
    color: #666;
    text-align: left;
  }
  .details div {
    display: flex;
    justify-content: space-between;
    padding: 0.25rem 0;
    border-bottom: 1px solid #222;
  }
  .details div:last-child { border-bottom: none; }
  .cta-card {
    background: #16213e;
    border-radius: 12px;
    padding: 1.5rem;
    margin-bottom: 1.5rem;
  }
  .cta-card p {
    margin-bottom: 1rem;
    color: #aaa;
    font-size: 0.95rem;
  }
  .cta-button {
    display: inline-block;
    background: #3b82f6;
    color: #fff;
    text-decoration: none;
    padding: 0.75rem 2rem;
    border-radius: 8px;
    font-weight: 600;
    font-size: 1rem;
    transition: background 0.2s;
  }
  .cta-button:hover {
    background: #2563eb;
  }
  .footer {
    font-size: 0.75rem;
    color: #555;
    margin-top: 2rem;
  }
</style>
</head>
<body>
<div class="container">
  <div class="logo">ForgeKey</div>
  <div class="subtitle">Cabinet Lock</div>

  <div class="status-card" id="statusCard">
    <div class="status-label">Lock Status</div>
    <div class="status-value" id="statusValue">--</div>
    <div class="details">
      <div><span>MAC</span><span id="macAddr">--</span></div>
      <div><span>Reed Switch</span><span id="reedStatus">--</span></div>
      <div><span>Latch Supervisor</span><span id="latchStatus">--</span></div>
      <div><span>IR Beam</span><span id="irStatus">--</span></div>
      <div><span>Mortise Switch</span><span id="mortiseStatus">--</span></div>
      <div><span>Uptime</span><span id="uptime">--</span></div>
    </div>
  </div>

  <div class="cta-card">
    <p>Need to unlock this cabinet? Generate an unlock code from the OpenMakerSuite dashboard.</p>
    <a href="https://openmakersuite.com" target="_blank" class="cta-button">
      Go to OpenMakerSuite
    </a>
  </div>

  <div class="footer">
    ForgeKey Cabinet Lock &mdash; Built with ESP32
  </div>
</div>

<script>
function updateStatus() {
  fetch('/api/status')
    .then(r => r.json())
    .then(data => {
      document.getElementById('statusValue').textContent = data.state || '--';
      document.getElementById('statusValue').className = 'status-value status-' + (data.state || '');
      document.getElementById('statusCard').className = 'status-card status-' + (data.state || '');
      document.getElementById('macAddr').textContent = data.mac || '--';
      document.getElementById('reedStatus').textContent = data.reed_closed ? 'Closed' : 'Open';
      document.getElementById('latchStatus').textContent = data.latch_locked ? 'Locked' : 'Unlocked';
      document.getElementById('irStatus').textContent = data.ir_broken ? 'Broken' : 'Intact';
      document.getElementById('mortiseStatus').textContent = data.mortise_active ? 'Active' : 'Inactive';
      var ms = data.uptime || 0;
      var h = Math.floor(ms / 3600000);
      var m = Math.floor((ms % 3600000) / 60000);
      var s = Math.floor((ms % 60000) / 1000);
      document.getElementById('uptime').textContent = h + 'h ' + m + 'm ' + s + 's';
    })
    .catch(err => {
      console.error('Failed to fetch status:', err);
    });
}
updateStatus();
setInterval(updateStatus, 5000);
</script>
</body>
</html>
)html";

static void handleRoot() {
    s_server->send(200, "text/html", HTML_PAGE);
}

static void handleApiStatus() {
    StaticJsonDocument<256> doc;
    doc["state"] = LockCapability::getStateName();
    doc["mac"] = WiFi.macAddress();

    auto tel = LockDevice::getTelemetry();
    doc["secure"] = tel.secure;
    doc["item_present"] = !tel.ir_broken;
    doc["uptime"] = tel.uptime_ms;
    doc["reed_closed"] = tel.reed_closed;
    doc["latch_locked"] = tel.latch_locked;
    doc["ir_broken"] = tel.ir_broken;
    doc["mortise_active"] = tel.mortise_active;

    String json;
    serializeJson(doc, json);
    s_server->send(200, "application/json", json);
}

void begin() {
    s_server = new WebServer(FORGEKEY_LOCK_WEB_PORT);

    s_server->on("/", handleRoot);
    s_server->on("/api/status", handleApiStatus);

    s_server->begin();
    s_started = true;

    Serial.printf("[LOCK/WEB] server started on port %d\n", FORGEKEY_LOCK_WEB_PORT);
    Serial.printf("[LOCK/WEB] connect to http://%s\n", WiFi.localIP().toString().c_str());
}

void tick() {
    if (!s_started || !s_server) return;
    s_server->handleClient();
}

void end() {
    if (s_server) {
        delete s_server;
        s_server = nullptr;
    }
    s_started = false;
}

String getStatusPageHtml() {
    return String(HTML_PAGE);
}

}  // namespace LockWeb
