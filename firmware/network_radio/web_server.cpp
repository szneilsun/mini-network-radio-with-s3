#include "web_server.h"

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config/audio_config.h"
#include "config/board_config.h"
#include "config/web_config.h"

namespace web {
namespace {

WebServer server(web_config::kHttpPort);
volatile bool *gTestToneEnabled = nullptr;
char accessPointSsid[20] = {};

// ESP.getEfuseMac() is represented little-endian by this Arduino core. Its
// low word is therefore the board's established setup identifier: B8:1F ->
// 1FB8.
uint16_t setupAccessPointId() {
  const uint64_t mac = ESP.getEfuseMac();
  return static_cast<uint16_t>(mac & 0xFFFFU);
}

constexpr char kIndexHtml[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>网络收音机 · 硬件测试</title>
<style>body{max-width:720px;margin:32px auto;padding:0 18px;font:16px system-ui;background:#101827;color:#e5e7eb}h1{margin-bottom:4px}p{color:#aab5c8}button{padding:10px 14px;margin:4px;border:0;border-radius:7px;background:#38bdf8;color:#062032;font-weight:700;cursor:pointer}button.off{background:#fbbf24}pre{padding:16px;overflow:auto;border-radius:8px;background:#172234;color:#cbd5e1;line-height:1.5}.hint{font-size:.9rem}</style>
<h1>ESP32-S3 网络收音机</h1><p>Step 2 · Web Server 最小框架</p>
<button id="on">开启测试音</button><button id="off" class="off">关闭测试音</button><button id="restart">重启设备</button>
<h2>实时状态</h2><pre id="status">正在读取…</pre><p class="hint">此版本仅开放本地热点管理页。Wi-Fi 配网、播放列表和 OTA 将在后续版本加入。</p>
<script>
const request=(url)=>fetch(url,{method:'POST'}).then(r=>r.json()).then(refresh);
document.querySelector('#on').onclick=()=>request('/api/audio/test?enabled=1');
document.querySelector('#off').onclick=()=>request('/api/audio/test?enabled=0');
document.querySelector('#restart').onclick=()=>fetch('/api/reboot',{method:'POST'});
async function refresh(){try{const s=await fetch('/api/status',{cache:'no-store'}).then(r=>r.json());document.querySelector('#status').textContent=JSON.stringify(s,null,2)}catch(e){document.querySelector('#status').textContent='连接失败：'+e}}
refresh();setInterval(refresh,2000);
</script></html>
)HTML";

void sendJson(const char *json, int statusCode = 200) {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(statusCode, "application/json; charset=utf-8", json);
}

void handleStatus() {
  char json[640];
  const uint32_t uptimeSeconds = millis() / 1000U;
  snprintf(
      json, sizeof(json),
      "{\"firmware\":\"%s\",\"uptime_seconds\":%lu,"
      "\"access_point\":{\"ssid\":\"%s\",\"ip\":\"%s\",\"clients\":%u},"
      "\"audio\":{\"output\":\"%s\",\"sample_rate_hz\":%lu,\"bits\":%u,\"channels\":%u,\"test_tone_enabled\":%s},"
      "\"memory\":{\"heap_free\":%u,\"heap_min_free\":%u,"
      "\"psram_total\":%u,\"psram_free\":%u,\"psram_min_free\":%u},"
      "\"flash\":{\"total\":%u,\"sketch\":%u,\"sketch_free\":%u}}",
      board::kFirmwareVersion, static_cast<unsigned long>(uptimeSeconds),
      accessPointSsid, WiFi.softAPIP().toString().c_str(),
      WiFi.softAPgetStationNum(),
      audio_config::kOutputDeviceName,
      static_cast<unsigned long>(audio_config::kSampleRateHz),
      audio_config::kBitsPerSample, audio_config::kChannels,
      (*gTestToneEnabled) ? "true" : "false", ESP.getFreeHeap(),
      ESP.getMinFreeHeap(), ESP.getPsramSize(), ESP.getFreePsram(),
      ESP.getMinFreePsram(), ESP.getFlashChipSize(), ESP.getSketchSize(),
      ESP.getFreeSketchSpace());
  sendJson(json);
}

void handleToneControl() {
  if (gTestToneEnabled == nullptr || !server.hasArg("enabled")) {
    sendJson("{\"error\":\"missing enabled parameter\"}", 400);
    return;
  }
  const String value = server.arg("enabled");
  if (value == "1" || value == "true") {
    *gTestToneEnabled = true;
  } else if (value == "0" || value == "false") {
    *gTestToneEnabled = false;
  } else {
    sendJson("{\"error\":\"enabled must be 0 or 1\"}", 400);
    return;
  }
  handleStatus();
}

void handleNotFound() {
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "Redirecting to / ");
}

}  // namespace

void begin(volatile bool *testToneEnabled) {
  gTestToneEnabled = testToneEnabled;

  snprintf(accessPointSsid, sizeof(accessPointSsid), "%s%04llX",
           web_config::kAccessPointPrefix,
           static_cast<unsigned long long>(setupAccessPointId()));

  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(accessPointSsid, web_config::kAccessPointPassword,
                   web_config::kAccessPointChannel, false,
                   web_config::kAccessPointMaxClients)) {
    Serial.println("ERROR: failed to start Wi-Fi access point.");
    return;
  }

  server.on("/", HTTP_GET,
            []() { server.send_P(200, "text/html; charset=utf-8", kIndexHtml); });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/audio/test", HTTP_POST, handleToneControl);
  server.on("/api/reboot", HTTP_POST, []() {
    sendJson("{\"restarting\":true}");
    delay(100);
    ESP.restart();
  });
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.printf("Web server ready: connect to %s (password: %s), then open http://%s/\n",
                accessPointSsid, web_config::kAccessPointPassword,
                WiFi.softAPIP().toString().c_str());
}

void handleClient() {
  server.handleClient();
}

}  // namespace web
