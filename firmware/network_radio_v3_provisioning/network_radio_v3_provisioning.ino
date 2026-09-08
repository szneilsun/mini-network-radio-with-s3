/*
 * Network Radio V3 - first-time Wi-Fi provisioning and AP fallback.
 *
 * Board: ESP32-S3-WROOM-1-N16R8 + MAX98357A
 * I2S: BCLK GPIO4, LRCLK GPIO5, DIN GPIO6
 *
 * This is intentionally a standalone sketch. It keeps the Step 2 hardware
 * tone while adding the network behaviour required before the radio player.
 */

#include <Arduino.h>
#include <DNSServer.h>
#include <ESP_I2S.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

namespace config {
constexpr char kFirmwareVersion[] = "0.3.0-provisioning";
constexpr uint32_t kSerialBaud = 115200;

constexpr gpio_num_t kI2sBclk = GPIO_NUM_4;
constexpr gpio_num_t kI2sLrclk = GPIO_NUM_5;
constexpr gpio_num_t kI2sDataOut = GPIO_NUM_6;
constexpr char kOutputDeviceName[] = "MAX98357A";
constexpr uint8_t kAmplifierSupplyVolts = 5;
constexpr uint32_t kSampleRateHz = 48000;
constexpr uint16_t kToneFrames = 256;
constexpr float kToneHz = 440.0F;
constexpr uint8_t kTonePercent = 8;

constexpr char kPreferencesNamespace[] = "radio";
constexpr char kStoredSsidKey[] = "wifi_ssid";
constexpr char kStoredPasswordKey[] = "wifi_pass";
constexpr char kAccessPointPrefix[] = "Radio-";
constexpr char kAccessPointPassword[] = "radio-setup";
constexpr uint8_t kAccessPointChannel = 6;
constexpr uint32_t kStationConnectTimeoutMs = 15000;
constexpr uint32_t kStationRecoveryTimeoutMs = 20000;
constexpr char kMdnsName[] = "network-radio";
}  // namespace config

namespace {

WebServer server(80);
DNSServer dnsServer;
Preferences preferences;
I2SClass i2s(I2S_NUM_0);

int16_t pcmBuffer[config::kToneFrames * 2];
char accessPointSsid[20] = {};
bool accessPointRunning = false;
bool stationConfigured = false;
bool mdnsRunning = false;
volatile bool testToneEnabled = true;
float tonePhase = 0.0F;
uint32_t framesProduced = 0;
uint32_t stationDisconnectedAt = 0;

constexpr float kTwoPi = 6.28318530718F;
constexpr float kToneStep =
    kTwoPi * config::kToneHz / config::kSampleRateHz;
constexpr int16_t kTonePeak =
    static_cast<int16_t>(32767L * config::kTonePercent / 100L);
constexpr uint32_t kFadeFrames = config::kSampleRateHz * 150U / 1000U;

// Arduino represents ESP.getEfuseMac() little-endian; its low word keeps the
// board's existing SSID rule: MAC B8:1F:... becomes 1FB8.
uint16_t setupAccessPointId() {
  const uint64_t mac = ESP.getEfuseMac();
  return static_cast<uint16_t>(mac & 0xFFFFU);
}

String jsonEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value[index];
    if (character == '\\' || character == '"') {
      escaped += '\\';
    }
    if (static_cast<uint8_t>(character) >= 0x20) {
      escaped += character;
    }
  }
  return escaped;
}

void sendJson(const String &body, int statusCode = 200) {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(statusCode, "application/json; charset=utf-8", body);
}

void startAccessPoint() {
  if (accessPointRunning) {
    return;
  }

  WiFi.mode(stationConfigured ? WIFI_AP_STA : WIFI_AP);
  if (!WiFi.softAP(accessPointSsid, config::kAccessPointPassword,
                   config::kAccessPointChannel, false, 4)) {
    Serial.println("ERROR: Wi-Fi access point failed to start.");
    return;
  }
  dnsServer.start(53, "*", WiFi.softAPIP());
  accessPointRunning = true;
  Serial.printf("Setup AP: %s / %s / http://%s\n", accessPointSsid,
                config::kAccessPointPassword,
                WiFi.softAPIP().toString().c_str());
}

void startMdns() {
  if (!mdnsRunning && MDNS.begin(config::kMdnsName)) {
    MDNS.addService("http", "tcp", 80);
    mdnsRunning = true;
    Serial.printf("mDNS ready: http://%s.local/\n", config::kMdnsName);
  }
}

bool connectSavedStation() {
  preferences.begin(config::kPreferencesNamespace, true);
  const String ssid = preferences.getString(config::kStoredSsidKey, "");
  const String password = preferences.getString(config::kStoredPasswordKey, "");
  preferences.end();

  if (ssid.isEmpty()) {
    Serial.println("No saved Wi-Fi network; entering setup mode.");
    return false;
  }

  Serial.printf("Connecting to saved Wi-Fi: %s\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), password.c_str());

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startedAt < config::kStationConnectTimeoutMs) {
    delay(250);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Saved Wi-Fi connection timed out; entering setup mode.");
    WiFi.disconnect();
    return false;
  }

  stationConfigured = true;
  stationDisconnectedAt = 0;
  Serial.printf("Station connected: %s\n", WiFi.localIP().toString().c_str());
  startMdns();
  return true;
}

void handleStatus() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  char json[720];
  snprintf(
      json, sizeof(json),
      "{\"firmware\":\"%s\",\"uptime_seconds\":%lu,"
      "\"network\":{\"station_configured\":%s,\"station_connected\":%s,"
      "\"station_ip\":\"%s\",\"setup_ap_running\":%s,"
      "\"setup_ap_ssid\":\"%s\",\"setup_ap_ip\":\"%s\",\"ap_clients\":%u},"
      "\"audio\":{\"output\":\"%s\",\"sample_rate_hz\":%lu,\"test_tone_enabled\":%s},"
      "\"memory\":{\"heap_free\":%u,\"heap_min_free\":%u,"
      "\"psram_total\":%u,\"psram_free\":%u},"
      "\"flash\":{\"total\":%u,\"sketch\":%u,\"sketch_free\":%u}}",
      config::kFirmwareVersion, static_cast<unsigned long>(millis() / 1000U),
      stationConfigured ? "true" : "false", connected ? "true" : "false",
      connected ? WiFi.localIP().toString().c_str() : "",
      accessPointRunning ? "true" : "false", accessPointSsid,
      accessPointRunning ? WiFi.softAPIP().toString().c_str() : "",
      accessPointRunning ? WiFi.softAPgetStationNum() : 0,
      config::kOutputDeviceName,
      static_cast<unsigned long>(config::kSampleRateHz),
      testToneEnabled ? "true" : "false", ESP.getFreeHeap(),
      ESP.getMinFreeHeap(), ESP.getPsramSize(), ESP.getFreePsram(),
      ESP.getFlashChipSize(), ESP.getSketchSize(), ESP.getFreeSketchSpace());
  sendJson(json);
}

void handleScan() {
  const int count = WiFi.scanNetworks(false, true);
  String json = "{\"networks\":[";
  for (int index = 0; index < count; ++index) {
    if (index > 0) {
      json += ',';
    }
    json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(index)) +
            "\",\"rssi\":" + String(WiFi.RSSI(index)) +
            ",\"secure\":" +
            String(WiFi.encryptionType(index) == WIFI_AUTH_OPEN ? "false" : "true") +
            '}';
  }
  json += "]}";
  WiFi.scanDelete();
  sendJson(json);
}

void handleSaveWifi() {
  const String ssid = server.arg("ssid");
  const String password = server.arg("password");
  if (ssid.isEmpty() || ssid.length() > 32 || password.length() > 63) {
    sendJson("{\"error\":\"invalid SSID or password length\"}", 400);
    return;
  }

  preferences.begin(config::kPreferencesNamespace, false);
  const size_t ssidWritten = preferences.putString(config::kStoredSsidKey, ssid);
  const size_t passwordWritten =
      preferences.putString(config::kStoredPasswordKey, password);
  preferences.end();
  if (ssidWritten != ssid.length() || passwordWritten != password.length()) {
    sendJson("{\"error\":\"failed to persist Wi-Fi settings\"}", 500);
    return;
  }

  sendJson("{\"saved\":true,\"restarting\":true}");
  delay(300);
  ESP.restart();
}

void handleForgetWifi() {
  preferences.begin(config::kPreferencesNamespace, false);
  preferences.clear();
  preferences.end();
  sendJson("{\"forgotten\":true,\"restarting\":true}");
  delay(300);
  ESP.restart();
}

void handleTone() {
  const String enabled = server.arg("enabled");
  if (enabled == "1") {
    testToneEnabled = true;
  } else if (enabled == "0") {
    testToneEnabled = false;
  } else {
    sendJson("{\"error\":\"enabled must be 0 or 1\"}", 400);
    return;
  }
  handleStatus();
}

constexpr char kIndexHtml[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>网络收音机配网</title><style>body{max-width:760px;margin:28px auto;padding:0 18px;font:16px system-ui;background:#101827;color:#e5e7eb}h1{margin-bottom:4px}section,pre{background:#172234;padding:16px;border-radius:9px;margin:16px 0}label{display:block;margin:10px 0}input,select,button{box-sizing:border-box;padding:10px;margin-top:5px;border-radius:6px;border:0}input,select{width:100%}button{background:#38bdf8;color:#062032;font-weight:700;cursor:pointer;margin:3px}button.warn{background:#fbbf24}pre{overflow:auto;line-height:1.45}</style>
<h1>ESP32-S3 网络收音机</h1><p>首次 Wi-Fi 配网与热点回退</p>
<section><h2>连接路由器</h2><button onclick="scan()">扫描 Wi-Fi</button><label>网络名称<select id="ssid"><option value="">选择或手动输入</option></select></label><label>密码<input id="pass" type="password" autocomplete="current-password"></label><button onclick="save()">保存并连接</button><p>保存后设备会自动重启；若连接失败，会再次启动本地配置热点。</p></section>
<section><h2>硬件测试</h2><button onclick="tone(1)">开启测试音</button><button class="warn" onclick="tone(0)">关闭测试音</button><button class="warn" onclick="forgetWifi()">清除 Wi-Fi 设置</button></section>
<h2>状态</h2><pre id="status">读取中…</pre>
<script>const q=s=>document.querySelector(s);async function refresh(){try{q('#status').textContent=JSON.stringify(await fetch('/api/status',{cache:'no-store'}).then(r=>r.json()),null,2)}catch(e){q('#status').textContent='连接失败：'+e}}async function scan(){const d=await fetch('/api/wifi/scan').then(r=>r.json()),s=q('#ssid');s.innerHTML='<option value="">选择网络</option>';d.networks.forEach(n=>s.innerHTML+='<option value="'+n.ssid.replaceAll('&','&amp;').replaceAll('"','&quot;')+'">'+n.ssid+' ('+n.rssi+' dBm'+(n.secure?'，加密':'，开放')+')</option>')}async function save(){const ssid=q('#ssid').value.trim(),password=q('#pass').value;if(!ssid){alert('请选择 Wi-Fi 网络');return}await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({ssid,password})});q('#status').textContent='设置已保存，设备正在重启…'}function tone(enabled){fetch('/api/audio/test?enabled='+enabled,{method:'POST'}).then(refresh)}function forgetWifi(){if(confirm('清除保存的 Wi-Fi 设置？'))fetch('/api/wifi/forget',{method:'POST'}).then(()=>q('#status').textContent='设置已清除，设备正在重启…')}refresh();setInterval(refresh,2000);</script></html>
)HTML";

void configureWebServer() {
  server.on("/", HTTP_GET,
            []() { server.send_P(200, "text/html; charset=utf-8", kIndexHtml); });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/wifi/scan", HTTP_GET, handleScan);
  server.on("/api/wifi", HTTP_POST, handleSaveWifi);
  server.on("/api/wifi/forget", HTTP_POST, handleForgetWifi);
  server.on("/api/audio/test", HTTP_POST, handleTone);
  server.on("/api/reboot", HTTP_POST, []() {
    sendJson("{\"restarting\":true}");
    delay(300);
    ESP.restart();
  });
  server.onNotFound([]() {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting");
  });
  server.begin();
}

void fillToneBuffer() {
  for (uint16_t frame = 0; frame < config::kToneFrames; ++frame) {
    const float gain = framesProduced < kFadeFrames
                           ? static_cast<float>(framesProduced) / kFadeFrames
                           : 1.0F;
    const int16_t sample = testToneEnabled
                               ? static_cast<int16_t>(sinf(tonePhase) * kTonePeak * gain)
                               : 0;
    pcmBuffer[frame * 2] = sample;
    pcmBuffer[frame * 2 + 1] = sample;
    tonePhase += kToneStep;
    if (tonePhase >= kTwoPi) {
      tonePhase -= kTwoPi;
    }
    ++framesProduced;
  }
}

void maintainStationConnection() {
  if (!stationConfigured || WiFi.status() == WL_CONNECTED) {
    stationDisconnectedAt = 0;
    return;
  }
  if (stationDisconnectedAt == 0) {
    stationDisconnectedAt = millis();
    WiFi.reconnect();
    Serial.println("Wi-Fi disconnected; attempting recovery.");
    return;
  }
  if (millis() - stationDisconnectedAt >= config::kStationRecoveryTimeoutMs) {
    Serial.println("Wi-Fi recovery timed out; enabling setup AP fallback.");
    startAccessPoint();
    stationDisconnectedAt = millis();
  }
}

}  // namespace

void setup() {
  Serial.begin(config::kSerialBaud);
  delay(300);
  Serial.printf("\n=== Network Radio %s ===\n", config::kFirmwareVersion);
  Serial.printf("Flash: %u bytes, PSRAM: %u bytes\n", ESP.getFlashChipSize(),
                ESP.getPsramSize());
  Serial.printf("Output: %s, VDD=%uV, speaker=OUT+/OUT- only\n",
                config::kOutputDeviceName, config::kAmplifierSupplyVolts);

  snprintf(accessPointSsid, sizeof(accessPointSsid), "%s%04llX",
           config::kAccessPointPrefix,
           static_cast<unsigned long long>(setupAccessPointId()));

  i2s.setPins(config::kI2sBclk, config::kI2sLrclk, config::kI2sDataOut);
  if (!i2s.begin(I2S_MODE_STD, config::kSampleRateHz,
                 I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                 I2S_STD_SLOT_BOTH)) {
    Serial.println("FATAL: I2S initialization failed.");
    while (true) {
      delay(1000);
    }
  }

  const bool connected = connectSavedStation();
  if (!connected) {
    startAccessPoint();
  }
  configureWebServer();
  Serial.println("I2S test tone and web control are ready.");
}

void loop() {
  if (accessPointRunning) {
    dnsServer.processNextRequest();
  }
  server.handleClient();
  maintainStationConnection();
  fillToneBuffer();
  i2s.write(pcmBuffer, sizeof(pcmBuffer));
  server.handleClient();
}
