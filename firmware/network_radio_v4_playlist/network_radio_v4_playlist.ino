/*
 * Network Radio V4 - Wi-Fi provisioning + persistent playlist management.
 * Hardware output: ESP32-S3 GPIO4/5/6 -> MAX98357A BCLK/LRCLK/DIN.
 */

#include <Arduino.h>
#include <DNSServer.h>
#include <ESP_I2S.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

namespace config {
#ifndef NETWORK_RADIO_VERSION
#define NETWORK_RADIO_VERSION "0.4.0-playlist"
#endif
constexpr char kFirmwareVersion[] = NETWORK_RADIO_VERSION;
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

constexpr char kWifiNamespace[] = "radio";
constexpr char kWifiSsidKey[] = "wifi_ssid";
constexpr char kWifiPasswordKey[] = "wifi_pass";
constexpr char kPlaylistNamespace[] = "playlist";
constexpr char kPlaylistCountKey[] = "count";
constexpr char kPlaylistSelectedKey[] = "selected";
#ifndef NETWORK_RADIO_MAX_STATIONS
#define NETWORK_RADIO_MAX_STATIONS 16
#endif
constexpr uint8_t kMaxStations = NETWORK_RADIO_MAX_STATIONS;
constexpr size_t kStationNameSize = 49;
constexpr size_t kStationUrlSize = 257;
constexpr size_t kStationLogoSize = 41;

constexpr char kAccessPointPrefix[] = "Radio-";
constexpr char kAccessPointPassword[] = "radio-setup";
constexpr uint8_t kAccessPointChannel = 6;
// Keep the setup hotspot visible during development. This makes an unheaded
// radio reachable even after it has joined a previously configured router.
constexpr bool kKeepSetupAccessPointAvailable = true;
constexpr uint32_t kStationConnectTimeoutMs = 15000;
constexpr uint32_t kStationRecoveryTimeoutMs = 20000;
constexpr char kMdnsName[] = "network-radio";
}  // namespace config

namespace {

struct Station {
  char name[config::kStationNameSize] = {};
  char url[config::kStationUrlSize] = {};
  char logo[config::kStationLogoSize] = {};
};

WebServer server(80);
DNSServer dnsServer;
Preferences preferences;
I2SClass i2s(I2S_NUM_0);
Station stations[config::kMaxStations];
uint8_t stationCount = 0;
uint8_t selectedStation = 0;

int16_t pcmBuffer[config::kToneFrames * 2];
char accessPointSsid[20] = {};
bool accessPointRunning = false;
bool stationConfigured = false;
bool mdnsRunning = false;
volatile bool testToneEnabled = true;
float tonePhase = 0.0F;
uint32_t framesProduced = 0;
uint32_t stationDisconnectedAt = 0;
void (*onStationSelected)(uint8_t) = nullptr;
void (*registerAdditionalRoutes)() = nullptr;

constexpr float kTwoPi = 6.28318530718F;
constexpr float kToneStep = kTwoPi * config::kToneHz / config::kSampleRateHz;
constexpr int16_t kTonePeak =
    static_cast<int16_t>(32767L * config::kTonePercent / 100L);
constexpr uint32_t kFadeFrames = config::kSampleRateHz * 150U / 1000U;

uint16_t setupAccessPointId() {
  // Arduino represents ESP.getEfuseMac() little-endian; B8:1F:... -> 1FB8.
  return static_cast<uint16_t>(ESP.getEfuseMac() & 0xFFFFU);
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

bool hasLineBreak(const String &value) {
  return value.indexOf('\n') >= 0 || value.indexOf('\r') >= 0;
}

void sendJson(const String &body, int statusCode = 200) {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(statusCode, "application/json; charset=utf-8", body);
}

String stationKey(uint8_t index) {
  return "item_" + String(index);
}

void setDefaultPlaylist() {
  stationCount = 1;
  selectedStation = 0;
  strlcpy(stations[0].name, "凤凰卫视音频", sizeof(stations[0].name));
  strlcpy(stations[0].url,
          "http://playtv-live.ifeng.com/live/06OLEEWQKN4_audio.m3u8",
          sizeof(stations[0].url));
}

bool persistPlaylist() {
  if (!preferences.begin(config::kPlaylistNamespace, false)) {
    return false;
  }
  preferences.clear();
  bool saved = preferences.putUChar(config::kPlaylistCountKey, stationCount) == 1 &&
               preferences.putUChar(config::kPlaylistSelectedKey, selectedStation) == 1;
  for (uint8_t index = 0; saved && index < stationCount; ++index) {
    const String entry = String(stations[index].name) + '\n' + stations[index].url + '\n' + stations[index].logo;
    saved = preferences.putString(stationKey(index).c_str(), entry) == entry.length();
  }
  preferences.end();
  return saved;
}

void loadPlaylist() {
  stationCount = 0;
  selectedStation = 0;
  if (!preferences.begin(config::kPlaylistNamespace, true)) {
    setDefaultPlaylist();
    return;
  }
  const uint8_t storedCount = preferences.getUChar(config::kPlaylistCountKey, 0);
  stationCount = min(storedCount, config::kMaxStations);
  selectedStation = preferences.getUChar(config::kPlaylistSelectedKey, 0);
  for (uint8_t index = 0; index < stationCount; ++index) {
    const String entry = preferences.getString(stationKey(index).c_str(), "");
    const int separator = entry.indexOf('\n');
    if (separator <= 0 || separator >= static_cast<int>(entry.length() - 1)) {
      stationCount = index;
      break;
    }
    strlcpy(stations[index].name, entry.substring(0, separator).c_str(),
            sizeof(stations[index].name));
    const int logoSeparator = entry.indexOf('\n', separator + 1);
    const String url = logoSeparator < 0 ? entry.substring(separator + 1) : entry.substring(separator + 1, logoSeparator);
    strlcpy(stations[index].url, url.c_str(),
            sizeof(stations[index].url));
    if (logoSeparator >= 0) strlcpy(stations[index].logo, entry.substring(logoSeparator + 1).c_str(), sizeof(stations[index].logo));
  }
  preferences.end();
  if (stationCount == 0) {
    setDefaultPlaylist();
    persistPlaylist();
  }
  if (selectedStation >= stationCount) {
    selectedStation = 0;
  }
}

String playlistJson() {
  String json = "{\"selected\":" + String(selectedStation) + ",\"stations\":[";
  for (uint8_t index = 0; index < stationCount; ++index) {
    if (index > 0) {
      json += ',';
    }
    json += "{\"id\":" + String(index) + ",\"name\":\"" +
            jsonEscape(stations[index].name) + "\",\"url\":\"" +
            jsonEscape(stations[index].url) + "\",\"logo\":\"" + jsonEscape(stations[index].logo) + "\"}";
  }
  return json + "]}";
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
  Serial.printf("Setup AP: %s / http://%s\n", accessPointSsid,
                WiFi.softAPIP().toString().c_str());
}

void startMdns() {
  if (!mdnsRunning && MDNS.begin(config::kMdnsName)) {
    MDNS.addService("http", "tcp", 80);
    mdnsRunning = true;
  }
}

bool connectSavedStation() {
  preferences.begin(config::kWifiNamespace, true);
  const String ssid = preferences.getString(config::kWifiSsidKey, "");
  const String password = preferences.getString(config::kWifiPasswordKey, "");
  preferences.end();
  if (ssid.isEmpty()) {
    return false;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), password.c_str());
  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startedAt < config::kStationConnectTimeoutMs) {
    delay(250);
  }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    return false;
  }
  stationConfigured = true;
  startMdns();
  return true;
}

void handleStatus() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  const char *name = stationCount ? stations[selectedStation].name : "";
  char json[760];
  snprintf(json, sizeof(json),
           "{\"firmware\":\"%s\",\"uptime_seconds\":%lu,"
           "\"network\":{\"station_connected\":%s,\"station_ip\":\"%s\","
           "\"setup_ap_ssid\":\"%s\",\"setup_ap_running\":%s},"
           "\"audio\":{\"output\":\"%s\",\"sample_rate_hz\":%lu,\"test_tone_enabled\":%s},"
           "\"player\":{\"selected_station\":%u,\"selected_name\":\"%s\",\"state\":\"playlist_only\"},"
           "\"memory\":{\"heap_free\":%u,\"psram_free\":%u}}",
           config::kFirmwareVersion, static_cast<unsigned long>(millis() / 1000U),
           connected ? "true" : "false",
           connected ? WiFi.localIP().toString().c_str() : "", accessPointSsid,
           accessPointRunning ? "true" : "false", config::kOutputDeviceName,
           static_cast<unsigned long>(config::kSampleRateHz),
           testToneEnabled ? "true" : "false", selectedStation, name,
           ESP.getFreeHeap(), ESP.getFreePsram());
  sendJson(json);
}

void handleWifiScan() {
  const int count = WiFi.scanNetworks(false, true);
  String json = "{\"networks\":[";
  for (int index = 0; index < count; ++index) {
    if (index) json += ',';
    json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(index)) + "\",\"rssi\":" +
            String(WiFi.RSSI(index)) + "}";
  }
  WiFi.scanDelete();
  sendJson(json + "]}");
}

void handleSaveWifi() {
  const String ssid = server.arg("ssid");
  const String password = server.arg("password");
  if (ssid.isEmpty() || ssid.length() > 32 || password.length() > 63) {
    sendJson("{\"error\":\"invalid SSID or password\"}", 400);
    return;
  }
  preferences.begin(config::kWifiNamespace, false);
  const bool saved = preferences.putString(config::kWifiSsidKey, ssid) == ssid.length() &&
                     preferences.putString(config::kWifiPasswordKey, password) == password.length();
  preferences.end();
  if (!saved) {
    sendJson("{\"error\":\"could not save Wi-Fi settings\"}", 500);
    return;
  }
  sendJson("{\"saved\":true,\"restarting\":true}");
  delay(300);
  ESP.restart();
}

void handleForgetWifi() {
  preferences.begin(config::kWifiNamespace, false);
  const bool cleared = preferences.clear();
  preferences.end();
  if (!cleared) {
    sendJson("{\"error\":\"could not clear Wi-Fi settings\"}", 500);
    return;
  }
  sendJson("{\"forgotten\":true,\"restarting\":true}");
  delay(300);
  ESP.restart();
}

bool parseStationId(uint8_t &id) {
  if (!server.hasArg("id")) return false;
  const int value = server.arg("id").toInt();
  if (value < 0 || value >= stationCount) return false;
  id = static_cast<uint8_t>(value);
  return true;
}

bool validateStation(const String &name, const String &url) {
  return !name.isEmpty() && name.length() < config::kStationNameSize &&
         !url.isEmpty() && url.length() < config::kStationUrlSize &&
         !hasLineBreak(name) && !hasLineBreak(url) &&
         (url.startsWith("http://") || url.startsWith("https://"));
}

void handleAddStation() {
  const String name = server.arg("name");
  const String url = server.arg("url");
  if (stationCount >= config::kMaxStations) {
    sendJson("{\"error\":\"playlist is full\"}", 409);
    return;
  }
  if (!validateStation(name, url)) {
    sendJson("{\"error\":\"name or URL is invalid\"}", 400);
    return;
  }
  strlcpy(stations[stationCount].name, name.c_str(), sizeof(stations[0].name));
  strlcpy(stations[stationCount].url, url.c_str(), sizeof(stations[0].url));
  ++stationCount;
  if (!persistPlaylist()) {
    sendJson("{\"error\":\"could not save playlist\"}", 500);
    return;
  }
  sendJson(playlistJson(), 201);
}

void handleUpdateStation() {
  uint8_t id;
  const String name = server.arg("name");
  const String url = server.arg("url");
  if (!parseStationId(id) || !validateStation(name, url)) {
    sendJson("{\"error\":\"invalid station data\"}", 400);
    return;
  }
  strlcpy(stations[id].name, name.c_str(), sizeof(stations[id].name));
  strlcpy(stations[id].url, url.c_str(), sizeof(stations[id].url));
  const bool saved = persistPlaylist();
  sendJson(saved ? playlistJson() : "{\"error\":\"could not save playlist\"}",
           saved ? 200 : 500);
}

void handleDeleteStation() {
  uint8_t id;
  if (!parseStationId(id) || stationCount <= 1) {
    sendJson("{\"error\":\"cannot delete this station\"}", 400);
    return;
  }
  for (uint8_t index = id; index + 1 < stationCount; ++index) {
    stations[index] = stations[index + 1];
  }
  --stationCount;
  if (selectedStation >= stationCount) selectedStation = stationCount - 1;
  const bool saved = persistPlaylist();
  sendJson(saved ? playlistJson() : "{\"error\":\"could not save playlist\"}",
           saved ? 200 : 500);
}

void handleSelectStation() {
  uint8_t id;
  if (!parseStationId(id)) {
    sendJson("{\"error\":\"invalid station id\"}", 400);
    return;
  }
  selectedStation = id;
  const bool saved = persistPlaylist();
  if (saved && onStationSelected != nullptr) {
    onStationSelected(selectedStation);
  }
  sendJson(saved ? playlistJson() : "{\"error\":\"could not save playlist\"}",
           saved ? 200 : 500);
}

void handleMoveStation() {
  uint8_t id;
  const String direction = server.arg("direction");
  if (!parseStationId(id) ||
      (direction != "up" && direction != "down") ||
      (direction == "up" && id == 0) ||
      (direction == "down" && id + 1 >= stationCount)) {
    sendJson("{\"error\":\"cannot move station\"}", 400);
    return;
  }
  const uint8_t other = direction == "up" ? id - 1 : id + 1;
  const Station selected = stations[id];
  stations[id] = stations[other];
  stations[other] = selected;
  if (selectedStation == id) selectedStation = other;
  else if (selectedStation == other) selectedStation = id;
  const bool saved = persistPlaylist();
  sendJson(saved ? playlistJson() : "{\"error\":\"could not save playlist\"}",
           saved ? 200 : 500);
}

void handleTone() {
  const String enabled = server.arg("enabled");
  if (enabled != "0" && enabled != "1") {
    sendJson("{\"error\":\"enabled must be 0 or 1\"}", 400);
    return;
  }
  testToneEnabled = enabled == "1";
  handleStatus();
}

constexpr char kIndexHtml[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>网络收音机 V4</title>
<style>body{max-width:850px;margin:28px auto;padding:0 18px;font:16px system-ui;background:#101827;color:#e5e7eb}section,pre,.station{background:#172234;padding:14px;border-radius:9px;margin:14px 0}input,select,button{box-sizing:border-box;padding:9px;margin:4px;border:0;border-radius:6px}input,select{width:100%}button{background:#38bdf8;color:#062032;font-weight:700}.warn{background:#fbbf24}.station small{display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:#b7c6da}.active{outline:2px solid #38bdf8}pre{overflow:auto}</style>
<h1>ESP32-S3 网络收音机</h1><p>V4 · 播放列表管理（播放引擎将在后续版本接入）</p>
<section><h2>播放列表</h2><div id="stations">加载中…</div><h3 id="formTitle">新增电台</h3><input id="editId" type="hidden"><input id="name" placeholder="电台名称"><input id="url" placeholder="http(s):// 音频流地址"><button onclick="saveStation()">保存</button><button class="warn" onclick="clearForm()">取消编辑</button></section>
<section><h2>Wi-Fi</h2><button onclick="scanWifi()">扫描网络</button><select id="ssid"><option>选择 Wi-Fi</option></select><input id="pass" type="password" placeholder="Wi-Fi 密码"><button onclick="saveWifi()">保存并连接</button><button class="warn" onclick="forgetWifi()">清除 Wi-Fi 设置</button></section>
<section><h2>硬件测试</h2><button onclick="post('/api/audio/test?enabled=1')">开启测试音</button><button class="warn" onclick="post('/api/audio/test?enabled=0')">关闭测试音</button></section><pre id="status">读取中…</pre>
<script>const q=s=>document.querySelector(s);const enc=o=>new URLSearchParams(o);async function api(u,o){let r=await fetch(u,o);let j=await r.json();if(!r.ok)throw Error(j.error);return j}async function refresh(){try{let[s,p]=await Promise.all([api('/api/status'),api('/api/stations')]);q('#status').textContent=JSON.stringify(s,null,2);render(p)}catch(e){q('#status').textContent='错误：'+e}}function render(p){q('#stations').innerHTML=p.stations.map(x=>`<div class="station ${x.id==p.selected?'active':''}"><b>${x.name}</b><small>${x.url}</small><button onclick="post('/api/stations/select?id=${x.id}')">设为默认</button><button onclick="edit(${x.id})">编辑</button><button onclick="post('/api/stations/move?id=${x.id}&direction=up')">↑</button><button onclick="post('/api/stations/move?id=${x.id}&direction=down')">↓</button><button class="warn" onclick="removeStation(${x.id})">删除</button></div>`).join('')}async function post(u){try{await api(u,{method:'POST'});refresh()}catch(e){alert(e)}}async function saveStation(){let id=q('#editId').value,body=enc({name:q('#name').value,url:q('#url').value});try{await api(id===''?'/api/stations':'/api/stations/update?id='+id,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});clearForm();refresh()}catch(e){alert(e)}}async function edit(id){let p=await api('/api/stations'),x=p.stations[id];q('#editId').value=id;q('#name').value=x.name;q('#url').value=x.url;q('#formTitle').textContent='编辑电台'}function clearForm(){q('#editId').value='';q('#name').value='';q('#url').value='';q('#formTitle').textContent='新增电台'}function removeStation(id){if(confirm('删除该电台？'))post('/api/stations/delete?id='+id)}async function scanWifi(){let d=await api('/api/wifi/scan'),s=q('#ssid');s.innerHTML='<option value="">选择 Wi-Fi</option>';d.networks.forEach(n=>s.innerHTML+=`<option value="${n.ssid}">${n.ssid} (${n.rssi} dBm)</option>`)}async function saveWifi(){try{await api('/api/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc({ssid:q('#ssid').value,password:q('#pass').value})});q('#status').textContent='Wi-Fi 已保存，设备正在重启…'}catch(e){alert(e)}}async function forgetWifi(){if(confirm('清除保存的 Wi-Fi？')){await api('/api/wifi/forget',{method:'POST'});q('#status').textContent='Wi-Fi 已清除，设备正在重启…'}}refresh();setInterval(refresh,2000)</script></html>
)HTML";

void configureWebServer() {
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html; charset=utf-8", kIndexHtml); });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/api/wifi", HTTP_POST, handleSaveWifi);
  server.on("/api/wifi/forget", HTTP_POST, handleForgetWifi);
  server.on("/api/stations", HTTP_GET, [] { sendJson(playlistJson()); });
  server.on("/api/stations", HTTP_POST, handleAddStation);
  server.on("/api/stations/update", HTTP_POST, handleUpdateStation);
  server.on("/api/stations/delete", HTTP_POST, handleDeleteStation);
  server.on("/api/stations/select", HTTP_POST, handleSelectStation);
  server.on("/api/stations/move", HTTP_POST, handleMoveStation);
  server.on("/api/audio/test", HTTP_POST, handleTone);
  server.onNotFound([] { server.sendHeader("Location", "/"); server.send(302, "text/plain", "Redirecting"); });
  if (registerAdditionalRoutes != nullptr) registerAdditionalRoutes();
  server.begin();
}

void fillToneBuffer() {
  for (uint16_t frame = 0; frame < config::kToneFrames; ++frame) {
    const float gain = framesProduced < kFadeFrames ? static_cast<float>(framesProduced) / kFadeFrames : 1.0F;
    const int16_t sample = testToneEnabled ? static_cast<int16_t>(sinf(tonePhase) * kTonePeak * gain) : 0;
    pcmBuffer[frame * 2] = sample;
    pcmBuffer[frame * 2 + 1] = sample;
    tonePhase += kToneStep;
    if (tonePhase >= kTwoPi) tonePhase -= kTwoPi;
    ++framesProduced;
  }
}

void maintainStationConnection() {
  if (!stationConfigured || WiFi.status() == WL_CONNECTED) {
    stationDisconnectedAt = 0;
    return;
  }
  if (!stationDisconnectedAt) {
    stationDisconnectedAt = millis();
    WiFi.reconnect();
  } else if (millis() - stationDisconnectedAt >= config::kStationRecoveryTimeoutMs) {
    startAccessPoint();
    stationDisconnectedAt = millis();
  }
}

}  // namespace

void setup() {
  Serial.begin(config::kSerialBaud);
  delay(300);
  snprintf(accessPointSsid, sizeof(accessPointSsid), "%s%04X", config::kAccessPointPrefix, setupAccessPointId());
  loadPlaylist();
  i2s.setPins(config::kI2sBclk, config::kI2sLrclk, config::kI2sDataOut);
  if (!i2s.begin(I2S_MODE_STD, config::kSampleRateHz, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    while (true) delay(1000);
  }
  const bool connected = connectSavedStation();
  if (!connected || config::kKeepSetupAccessPointAvailable) startAccessPoint();
  configureWebServer();
  Serial.printf("Network Radio %s ready; %u station(s) stored.\n", config::kFirmwareVersion, stationCount);
}

void loop() {
  if (accessPointRunning) dnsServer.processNextRequest();
  server.handleClient();
  maintainStationConnection();
  fillToneBuffer();
  i2s.write(pcmBuffer, sizeof(pcmBuffer));
}
