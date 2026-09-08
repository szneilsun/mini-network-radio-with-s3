/* Network Radio 2.0: player UI, administration UI, and WS2812B status LED. */

#define NETWORK_RADIO_VERSION "2.0.1"
#define NETWORK_RADIO_MAX_STATIONS 140
#ifdef NETWORK_RADIO_NO_ENTRYPOINT
#define NETWORK_RADIO_V8_NO_ENTRYPOINT
#endif
#define NETWORK_RADIO_NO_ENTRYPOINT
#include "../network_radio_v5_playback/network_radio_v5_playback.ino"
#undef NETWORK_RADIO_NO_ENTRYPOINT

#include "builtin_stations.h"
#include "station_icons.h"
#include <LittleFS.h>

#include <Update.h>

namespace {

constexpr char kSecurityNamespace[] = "security";
constexpr char kAdminPasswordKey[] = "admin_pass";
constexpr char kUiNamespace[] = "ui";
constexpr char kAdminUser[] = "admin";
constexpr uint8_t kLogCapacity = 36;
constexpr uint32_t kWifiRetryMs = 10000;
constexpr uint32_t kPlaybackRetryInitialMs = 8000;
constexpr uint32_t kPlaybackRetryMaxMs = 60000;
constexpr uint8_t kStatusLedPin = 48;
constexpr uint8_t kStatusLedBrightness = 36;
constexpr uint32_t kStatusLedRefreshMs = 20;
constexpr size_t kLegacyBuiltinStationCount = 100;
constexpr char kLegacyBuiltinCatalogKey[] = "builtin_100_v1";
constexpr char kNewsStationPackKey[] = "news_pack_v1";
constexpr char kRegionSortKey[] = "region_sort_v3";
static_assert(kBuiltinStationCount == 113,
              "Update the incremental station-pack boundary when the catalog changes.");

char adminPassword[64] = {};
char uiBackground[8] = "#656b6a";
char uiAccent[8] = "#f2a51a";
char uiTexture[16] = "none";
char logs[kLogCapacity][120] = {};
uint8_t logHead = 0;
uint8_t logCount = 0;
uint32_t lastWifiRetryAt = 0;
uint32_t nextPlaybackRetryAt = 0;
uint8_t playbackFailures = 0;
bool wasStationConnected = false;
bool otaSucceeded = false;
bool playbackEnabled = true;
bool stationChangePending = false;
bool otaInProgress = false;
bool statusLedError = false;
const char *pendingPlaybackReason = "station selected";
String otaError;

enum class StatusLedMode : uint8_t { Off, Buffering, Playing, Error, Ota };

bool audioMessageIndicatesError(const char *message) {
  if (message == nullptr) return false;
  String text(message);
  text.toLowerCase();
  return text.indexOf("error") >= 0 || text.indexOf("failed") >= 0 ||
         text.indexOf("forbidden") >= 0 || text.indexOf("timeout") >= 0 ||
         text.indexOf(" 403 ") >= 0 || text.indexOf(" 404 ") >= 0;
}

void updateStatusLed(bool force = false) {
  static uint32_t lastUpdateAt = 0;
  static uint32_t lastColor = UINT32_MAX;
  const uint32_t now = millis();
  if (!force && now - lastUpdateAt < kStatusLedRefreshMs) return;
  lastUpdateAt = now;

  if (audio.isRunning()) statusLedError = false;
  StatusLedMode mode = StatusLedMode::Off;
  if (otaInProgress) mode = StatusLedMode::Ota;
  else if (statusLedError || (stationConfigured && WiFi.status() != WL_CONNECTED)) mode = StatusLedMode::Error;
  else if (audio.isRunning()) mode = StatusLedMode::Playing;
  else if (playbackEnabled) mode = StatusLedMode::Buffering;

  uint8_t red = 0, green = 0, blue = 0;
  if (mode == StatusLedMode::Buffering || mode == StatusLedMode::Playing) {
    constexpr uint32_t periodMs = 4000;
    constexpr uint32_t halfPeriodMs = periodMs / 2;
    const uint32_t position = now % periodMs;
    const uint16_t ramp = position < halfPeriodMs
                              ? position * 255U / halfPeriodMs
                              : (periodMs - position) * 255U / halfPeriodMs;
    const uint8_t level = 1U + static_cast<uint32_t>(ramp) * ramp *
                                  (kStatusLedBrightness - 1U) / 65025U;
    if (mode == StatusLedMode::Buffering) red = level;
    else blue = level;
  } else if (mode == StatusLedMode::Error) {
    red = (now % 300U) < 150U ? kStatusLedBrightness : 0;
  } else if (mode == StatusLedMode::Ota) {
    green = (now % 1200U) < 600U ? kStatusLedBrightness : 0;
  }

  const uint32_t color = (static_cast<uint32_t>(red) << 16) |
                         (static_cast<uint32_t>(green) << 8) | blue;
  if (force || color != lastColor) {
    rgbLedWrite(kStatusLedPin, red, green, blue);
    lastColor = color;
  }
}

void enrichStationIcons() {
  bool changed = false;
  for (uint8_t stationIndex = 0; stationIndex < stationCount; ++stationIndex) {
    for (size_t iconIndex = 0; iconIndex < kIconStationCount; ++iconIndex) {
      const IconStation &icon = kIconStations[iconIndex];
      if (strcmp(stations[stationIndex].url, icon.url) != 0) continue;
      if (strcmp(stations[stationIndex].logo, icon.logo) != 0) {
        strlcpy(stations[stationIndex].logo, icon.logo,
                sizeof(stations[stationIndex].logo));
        changed = true;
      }
      break;
    }
  }
  if (changed) persistPlaylist();
}

bool stationAlreadyStored(const BuiltinStation &candidate) {
  for (uint8_t stored = 0; stored < stationCount; ++stored) {
    if (strcmp(stations[stored].url, candidate.url) == 0 ||
        strcmp(stations[stored].name, candidate.name) == 0) return true;
  }
  return false;
}

bool importBuiltinRange(size_t first, size_t end, bool &complete) {
  bool changed = false;
  complete = true;
  for (size_t source = first; source < end; ++source) {
    if (stationAlreadyStored(kBuiltinStations[source])) continue;
    if (stationCount >= config::kMaxStations) {
      complete = false;
      break;
    }
    stations[stationCount] = Station{};
    strlcpy(stations[stationCount].name, kBuiltinStations[source].name,
            sizeof(stations[stationCount].name));
    strlcpy(stations[stationCount].url, kBuiltinStations[source].url,
            sizeof(stations[stationCount].url));
    ++stationCount;
    changed = true;
  }
  if (changed && !persistPlaylist()) complete = false;
  return changed;
}

void importBuiltinStations() {
  Preferences importPreferences;
  importPreferences.begin("catalog", false);
  const bool imported = importPreferences.getBool(kLegacyBuiltinCatalogKey, false);
  importPreferences.end();
  if (imported) return;

  bool complete = false;
  importBuiltinRange(0, kBuiltinStationCount, complete);
  if (complete) {
    importPreferences.begin("catalog", false);
    importPreferences.putBool(kLegacyBuiltinCatalogKey, true);
    importPreferences.end();
  }
}

bool importNewsStationPack() {
  Preferences importPreferences;
  importPreferences.begin("catalog", false);
  const bool imported = importPreferences.getBool(kNewsStationPackKey, false);
  importPreferences.end();
  if (imported) return false;

  bool complete = false;
  const bool changed = importBuiltinRange(kLegacyBuiltinStationCount,
                                          kBuiltinStationCount, complete);
  if (complete) {
    importPreferences.begin("catalog", false);
    importPreferences.putBool(kNewsStationPackKey, true);
    importPreferences.end();
  }
  return changed;
}

void migrateStationCatalog() {
  struct StationMigration {
    const char *oldName;
    const char *oldUrl;
    const char *newName;
    const char *newUrl;
  };
  constexpr StationMigration migrations[] = {
    {nullptr, "https://radio.0472.org/?id=639", nullptr,
     "https://ngcdn001.cnr.cn/live/zgzs/index.m3u8"},
    {nullptr, "https://radio.0472.org/?id=640", nullptr,
     "https://ngcdn002.cnr.cn/live/jjzs/index.m3u8"},
    {"凤凰卫视音频", "http://playtv-live.ifeng.com/live/06OLEEWQKN4_audio.m3u8",
     nullptr, "https://playtv-live.ifeng.com/live/06OLEEWQKN4_audio.m3u8"},
    {"CRI环球资讯", "http://sk.cri.cn/905.m3u8", nullptr,
     "https://sk.cri.cn/905.m3u8"},
    {"北京新闻广播", "http://ls.qingting.fm/live/339.m3u8", nullptr,
     "https://lhttp.qtfm.cn/live/339/64k.mp3"},
    {"BCC News Network", "http://stream.rcs.revma.com/78fm9wyy2tzuv",
     "台湾中广新闻网", "https://n03.rcs.revma.com/78fm9wyy2tzuv"},
    {"Capital FM", "https://19183.live.streamtheworld.com/CAPITAL958FM_PREM.aac",
     "新加坡 CAPITAL 958",
     "https://playerservices.streamtheworld.com/api/livestream-redirect/CAPITAL958FM_PREM.aac"},
    {"RTHK普通话",
     "https://rthkradiopth-live.akamaized.net/hls/live/2040082/radiopth/master.m3u8",
     "香港电台普通话台",
     "https://rthkradiopth-live.akamaized.net/hls/live/2040082/radiopth/master.m3u8"},
    {"Radio France Interntional",
     "https://rfienchinois64k.ice.infomaniak.ch/rfienchinois-64.mp3",
     "RFI 法广中文",
     "https://rfienchinois64k.ice.infomaniak.ch/rfienchinois-64.mp3"},
  };

  bool changed = false;
  for (uint8_t index = 0; index < stationCount; ++index) {
    for (const StationMigration &migration : migrations) {
      if (strcmp(stations[index].url, migration.oldUrl) == 0) {
        if (strcmp(stations[index].url, migration.newUrl) != 0) {
          strlcpy(stations[index].url, migration.newUrl, sizeof(stations[index].url));
          changed = true;
        }
        if (migration.oldName != nullptr && migration.newName != nullptr &&
            strcmp(stations[index].name, migration.oldName) == 0) {
          strlcpy(stations[index].name, migration.newName, sizeof(stations[index].name));
          changed = true;
        }
        break;
      }
    }
  }
  if (changed) persistPlaylist();
}

struct RegionPrefix {
  const char *prefix;
  uint8_t order;
};

constexpr RegionPrefix kRegionOrder[] = {
  {"CNR", 0}, {"CRI", 0}, {"CCTV", 0}, {"凤凰", 0},
  {"北京", 1}, {"天津", 2}, {"上海", 3}, {"重庆", 4},
  {"河北", 5}, {"山西", 6}, {"内蒙", 7}, {"辽宁", 8},
  {"吉林", 9}, {"黑龙江", 10}, {"龙江", 10},
  {"江苏", 11}, {"南京", 11}, {"浙江", 12}, {"安徽", 13},
  {"福建", 14}, {"厦门", 14}, {"江西", 15}, {"山东", 16},
  {"河南", 17}, {"湖北", 18}, {"湖南", 19}, {"长沙", 19}, {"芒果", 19},
  {"广东", 20}, {"深圳", 20}, {"广西", 21}, {"海南", 22},
  {"四川", 23}, {"贵州", 24}, {"云南", 25}, {"西藏", 26},
  {"陕西", 27}, {"甘肃", 28}, {"青海", 29}, {"宁夏", 30},
  {"新疆", 31}, {"香港", 32}, {"RTHK", 32}, {"澳门", 33}, {"台湾", 34},
  {"新加坡", 35}, {"马来西亚", 36}, {"RFI", 37},
};

uint8_t stationRegionOrder(const char *name) {
  for (const RegionPrefix &region : kRegionOrder) {
    if (strncmp(name, region.prefix, strlen(region.prefix)) == 0) return region.order;
  }
  return 250;
}

bool stationComesAfter(const Station &left, const Station &right) {
  const bool leftIsPhoenix = strcmp(left.name, "凤凰卫视音频") == 0;
  const bool rightIsPhoenix = strcmp(right.name, "凤凰卫视音频") == 0;
  if (leftIsPhoenix != rightIsPhoenix) return !leftIsPhoenix;
  const uint8_t leftRegion = stationRegionOrder(left.name);
  const uint8_t rightRegion = stationRegionOrder(right.name);
  if (leftRegion != rightRegion) return leftRegion > rightRegion;
  return strcmp(left.name, right.name) > 0;
}

void sortStationsByRegionOnce(bool forceSort = false) {
  Preferences catalogPreferences;
  catalogPreferences.begin("catalog", false);
  const bool alreadySorted = catalogPreferences.getBool(kRegionSortKey, false);
  catalogPreferences.end();
  if (alreadySorted && !forceSort) return;

  uint8_t trackedSelection = selectedStation;
  for (uint8_t index = 1; index < stationCount; ++index) {
    Station current = stations[index];
    const bool movingSelected = trackedSelection == index;
    int position = index - 1;
    while (position >= 0 && stationComesAfter(stations[position], current)) {
      stations[position + 1] = stations[position];
      if (!movingSelected && trackedSelection == position) trackedSelection = position + 1;
      --position;
    }
    stations[position + 1] = current;
    if (movingSelected) trackedSelection = position + 1;
  }
  selectedStation = trackedSelection;

  if (persistPlaylist()) {
    catalogPreferences.begin("catalog", false);
    catalogPreferences.putBool(kRegionSortKey, true);
    catalogPreferences.end();
  }
}

void addLog(const char *kind, const char *message) {
  snprintf(logs[logHead], sizeof(logs[logHead]), "%lus [%s] %.92s",
           static_cast<unsigned long>(millis() / 1000U), kind, message ? message : "");
  logHead = (logHead + 1) % kLogCapacity;
  if (logCount < kLogCapacity) ++logCount;
  Serial.println(logs[(logHead + kLogCapacity - 1) % kLogCapacity]);
}

void audioInfoV8(Audio::msg_t message) {
  if (message.msg != nullptr) {
    setPlayerMessage(message.msg);
    addLog("audio", message.msg);
    if (audioMessageIndicatesError(message.msg)) statusLedError = true;
  }
  if (message.s != nullptr && message.msg != nullptr) {
    Serial.printf("audio %s: %s\n", message.s, message.msg);
  }
}

void loadSecurity() {
  playerPreferences.begin(kSecurityNamespace, true);
  const String stored = playerPreferences.getString(kAdminPasswordKey, "");
  playerPreferences.end();
  strlcpy(adminPassword, stored.c_str(), sizeof(adminPassword));
}

bool isHexColor(const String &value) {
  if (value.length() != 7 || value[0] != '#') return false;
  for (uint8_t index = 1; index < 7; ++index) {
    const char c = value[index];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

bool isKnownTexture(const String &value) {
  return value == "none" || value == "dots" || value == "grid" ||
         value == "diagonal" || value == "cloud" || value == "lattice" ||
         value == "waves" || value == "bamboo" || value == "ricepaper" ||
         value == "porcelain";
}

void loadUiTheme() {
  Preferences uiPreferences;
  uiPreferences.begin(kUiNamespace, true);
  const String background = uiPreferences.getString("background", uiBackground);
  const String accent = uiPreferences.getString("accent", uiAccent);
  const String texture = uiPreferences.getString("texture", uiTexture);
  uiPreferences.end();
  if (isHexColor(background)) strlcpy(uiBackground, background.c_str(), sizeof(uiBackground));
  if (isHexColor(accent)) strlcpy(uiAccent, accent.c_str(), sizeof(uiAccent));
  if (isKnownTexture(texture)) strlcpy(uiTexture, texture.c_str(), sizeof(uiTexture));
}

String uiThemeJson() {
  return "{\"background\":\"" + String(uiBackground) +
         "\",\"accent\":\"" + String(uiAccent) +
         "\",\"texture\":\"" + String(uiTexture) + "\"}";
}

bool requireAdmin();

void handleSaveUiTheme() {
  if (!requireAdmin()) return;
  const String background = server.arg("background");
  const String accent = server.arg("accent");
  const String texture = server.arg("texture");
  if (!isHexColor(background) || !isHexColor(accent) || !isKnownTexture(texture)) {
    sendJson("{\"error\":\"invalid UI theme\"}", 400);
    return;
  }
  Preferences uiPreferences;
  uiPreferences.begin(kUiNamespace, false);
  const bool saved = uiPreferences.putString("background", background) == background.length() &&
                     uiPreferences.putString("accent", accent) == accent.length() &&
                     uiPreferences.putString("texture", texture) == texture.length();
  uiPreferences.end();
  if (!saved) {
    sendJson("{\"error\":\"could not save UI theme\"}", 500);
    return;
  }
  strlcpy(uiBackground, background.c_str(), sizeof(uiBackground));
  strlcpy(uiAccent, accent.c_str(), sizeof(uiAccent));
  strlcpy(uiTexture, texture.c_str(), sizeof(uiTexture));
  sendJson(uiThemeJson());
}

bool isAdminRequest() {
  return adminPassword[0] == '\0' || server.authenticate(kAdminUser, adminPassword);
}

bool requireAdmin() {
  if (isAdminRequest()) return true;
  server.requestAuthentication(BASIC_AUTH, "Network Radio 2.0 Admin");
  return false;
}

bool containsLineBreak(const String &value) {
  return value.indexOf('\n') >= 0 || value.indexOf('\r') >= 0;
}

void schedulePlaybackRetry(const char *reason) {
  if (playbackFailures < 10) ++playbackFailures;
  const uint8_t shift = min<uint8_t>(playbackFailures - 1, 3);
  const uint32_t waitMs = min<uint32_t>(kPlaybackRetryInitialMs << shift, kPlaybackRetryMaxMs);
  nextPlaybackRetryAt = millis() + waitMs;
  playerRequested = false;
  statusLedError = true;
  setPlayerMessage(reason);
  addLog("recovery", reason);
}

bool startSelectedStationV8(const char *reason) {
  if (stationCount == 0 || WiFi.status() != WL_CONNECTED) {
    schedulePlaybackRetry("waiting for router Wi-Fi");
    return false;
  }
  audio.stopSong();
  delay(40);
  // ESP32-audioI2S keeps its I2S channel alive after the first setPinout().
  // Re-registering that channel causes an ESP_ERR_INVALID_STATE abort, so a
  // recovery rebuild stops the decoder and reconnects only; setup owns I2S init.
  playerRequested = audio.connecttohost(stations[selectedStation].url);
  if (!playerRequested) {
    schedulePlaybackRetry("could not start audio stream");
    return false;
  }
  statusLedError = false;
  playbackFailures = 0;
  nextPlaybackRetryAt = millis() + 20000;
  setPlayerMessage("connecting");
  addLog("player", reason);
  return true;
}

void onPlaylistSelectionV8(uint8_t) {
  playbackEnabled = true;
  stationChangePending = true;
  pendingPlaybackReason = "station selected";
  playerRequested = true;
  statusLedError = false;
  setPlayerMessage("switching station");
}

void maintainNetworkAndPlayback() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (!connected) {
    if (wasStationConnected) {
      audio.stopSong();
      playerRequested = false;
      setPlayerMessage("router Wi-Fi disconnected; retrying");
      addLog("wifi", "router Wi-Fi disconnected");
    }
    if (millis() - lastWifiRetryAt >= kWifiRetryMs) {
      lastWifiRetryAt = millis();
      WiFi.reconnect();
      addLog("wifi", "reconnect requested");
    }
    startAccessPoint();
    wasStationConnected = false;
    return;
  }
  if (!wasStationConnected) {
    wasStationConnected = true;
    addLog("wifi", "router Wi-Fi connected");
    nextPlaybackRetryAt = millis() + 500;
  }
  // Run the potentially slow network connection only after the HTTP handler
  // has returned its response, so the selected station updates immediately.
  if (stationChangePending) {
    stationChangePending = false;
    startSelectedStationV8(pendingPlaybackReason);
    return;
  }
  // A stream that remains non-running past its grace period is rebuilt from
  // the playlist URL. This also forces HLS to fetch the current media sequence.
  if (playbackEnabled && !audio.isRunning() && millis() >= nextPlaybackRetryAt) {
    startSelectedStationV8("rebuilding playback chain");
  }
}

String diagnosticsJson() {
  String json = "{\"entries\":[";
  for (uint8_t n = 0; n < logCount; ++n) {
    const uint8_t index = (logHead + kLogCapacity - logCount + n) % kLogCapacity;
    if (n) json += ',';
    json += "\"" + jsonEscape(logs[index]) + "\"";
  }
  return json + "]}";
}

void handleStatusV8() {
  if (!requireAdmin()) return;
  const bool connected = WiFi.status() == WL_CONNECTED;
  const char *state = audio.isRunning() ? "playing" :
                      (playerRequested ? "buffering" : "recovering_or_stopped");
  String json = "{\"firmware\":\"" + String(config::kFirmwareVersion) +
                "\",\"network\":{\"station_connected\":" +
                String(connected ? "true" : "false") + ",\"station_ip\":\"" +
                (connected ? WiFi.localIP().toString() : "") + "\",\"setup_ap_ssid\":\"" +
                String(accessPointSsid) + "\",\"setup_ap_password_is_separate\":true},\"player\":{\"state\":\"" +
                state + "\",\"selected_name\":\"" + jsonEscape(stations[selectedStation].name) +
                "\",\"volume\":" + String(playerVolume) + ",\"message\":\"" +
                jsonEscape(playerMessage) + "\",\"failures\":" + String(playbackFailures) +
                "},\"security\":{\"management_password_enabled\":" +
                String(adminPassword[0] ? "true" : "false") + "},\"memory\":{\"heap_free\":" +
                String(ESP.getFreeHeap()) + ",\"psram_free\":" + String(ESP.getFreePsram()) + "}}";
  sendJson(json);
}

void handlePlayerStatusV8() { if (requireAdmin()) handlePlayerStatus(); }
void handlePlayerPlayV8() {
  if (!requireAdmin()) return;
  playbackEnabled = true;
  stationChangePending = true;
  pendingPlaybackReason = "play requested";
  playerRequested = true;
  statusLedError = false;
  setPlayerMessage("connecting");
  handlePlayerStatus();
}
void handlePlayerStopV8() { if (requireAdmin()) { playbackEnabled = false; stationChangePending = false; statusLedError = false; audio.stopSong(); playerRequested = false; setPlayerMessage("stopped by user"); addLog("player", "stopped by user"); handlePlayerStatus(); } }
void handlePlayerVolumeV8() { if (requireAdmin()) handlePlayerVolume(); }

void handlePlayerPreviousV9() {
  if (!requireAdmin()) return;
  if (stationCount == 0) { sendJson("{\"error\":\"playlist is empty\"}", 409); return; }
  playbackEnabled = true;
  selectedStation = selectedStation == 0 ? stationCount - 1 : selectedStation - 1;
  persistPlaylist();
  stationChangePending = true;
  pendingPlaybackReason = "previous station";
  playerRequested = true;
  statusLedError = false;
  setPlayerMessage("switching station");
  handlePlayerStatus();
}

void handlePlayerNextV9() {
  if (!requireAdmin()) return;
  if (stationCount == 0) { sendJson("{\"error\":\"playlist is empty\"}", 409); return; }
  playbackEnabled = true;
  selectedStation = (selectedStation + 1) % stationCount;
  persistPlaylist();
  stationChangePending = true;
  pendingPlaybackReason = "next station";
  playerRequested = true;
  statusLedError = false;
  setPlayerMessage("switching station");
  handlePlayerStatus();
}

String userPlaylistJson() {
  String json = "{\"selected\":" + String(selectedStation) + ",\"stations\":[";
  for (uint8_t index = 0; index < stationCount; ++index) {
    if (index) json += ',';
    json += "{\"id\":" + String(index) + ",\"name\":\"" +
            jsonEscape(stations[index].name) + "\",\"logo\":\"" +
            jsonEscape(stations[index].logo) + "\"}";
  }
  return json + "]}";
}

void handleUserPlay() {
  playbackEnabled = true;
  stationChangePending = true;
  pendingPlaybackReason = "play requested from user page";
  playerRequested = true;
  statusLedError = false;
  setPlayerMessage("connecting");
  handlePlayerStatus();
}

void handleUserStop() {
  playbackEnabled = false;
  stationChangePending = false;
  statusLedError = false;
  audio.stopSong();
  playerRequested = false;
  setPlayerMessage("stopped by user");
  addLog("player", "stopped from user page");
  handlePlayerStatus();
}

void handleUserPrevious() {
  if (stationCount == 0) { sendJson("{\"error\":\"playlist is empty\"}", 409); return; }
  playbackEnabled = true;
  selectedStation = selectedStation == 0 ? stationCount - 1 : selectedStation - 1;
  persistPlaylist();
  stationChangePending = true;
  pendingPlaybackReason = "previous station from user page";
  playerRequested = true;
  statusLedError = false;
  setPlayerMessage("switching station");
  handlePlayerStatus();
}

void handleUserNext() {
  if (stationCount == 0) { sendJson("{\"error\":\"playlist is empty\"}", 409); return; }
  playbackEnabled = true;
  selectedStation = (selectedStation + 1) % stationCount;
  persistPlaylist();
  stationChangePending = true;
  pendingPlaybackReason = "next station from user page";
  playerRequested = true;
  statusLedError = false;
  setPlayerMessage("switching station");
  handlePlayerStatus();
}

void handleMoveStationV11() {
  uint8_t id;
  if (!parseStationId(id)) {
    sendJson("{\"error\":\"invalid station id\"}", 400);
    return;
  }

  const String direction = server.arg("direction");
  uint8_t target = id;
  if (direction == "up" && id > 0) target = id - 1;
  else if (direction == "down" && id + 1 < stationCount) target = id + 1;
  else if (direction == "first") target = 0;
  else if (direction == "last") target = stationCount - 1;
  else if (direction != "up" && direction != "down") {
    sendJson("{\"error\":\"invalid move direction\"}", 400);
    return;
  }

  if (target != id) {
    const Station moved = stations[id];
    if (target < id) {
      for (int index = id; index > target; --index) stations[index] = stations[index - 1];
      if (selectedStation >= target && selectedStation < id) ++selectedStation;
    } else {
      for (uint8_t index = id; index < target; ++index) stations[index] = stations[index + 1];
      if (selectedStation > id && selectedStation <= target) --selectedStation;
    }
    stations[target] = moved;
    if (selectedStation == id) selectedStation = target;
  }

  const bool saved = persistPlaylist();
  sendJson(saved ? playlistJson() : "{\"error\":\"could not save playlist\"}",
           saved ? 200 : 500);
}

void handleDiagnostics() { if (requireAdmin()) sendJson(diagnosticsJson()); }
void handleDiagnosticsDownload() {
  if (!requireAdmin()) return;
  String text = "Network Radio " + String(config::kFirmwareVersion) + " diagnostics\n";
  for (uint8_t n = 0; n < logCount; ++n) text += String(logs[(logHead + kLogCapacity - logCount + n) % kLogCapacity]) + '\n';
  server.sendHeader("Content-Disposition", "attachment; filename=network-radio-diagnostics.txt");
  server.send(200, "text/plain; charset=utf-8", text);
}

void handleSetPassword() {
  if (!requireAdmin()) return;
  const String password = server.arg("password");
  if (password.length() < 8 || password.length() > 63 || containsLineBreak(password)) {
    sendJson("{\"error\":\"password must be 8..63 characters\"}", 400); return;
  }
  playerPreferences.begin(kSecurityNamespace, false);
  const bool saved = playerPreferences.putString(kAdminPasswordKey, password) == password.length();
  playerPreferences.end();
  if (!saved) { sendJson("{\"error\":\"could not save password\"}", 500); return; }
  strlcpy(adminPassword, password.c_str(), sizeof(adminPassword));
  addLog("security", "management password enabled");
  sendJson("{\"saved\":true,\"username\":\"admin\"}");
}

void handleFactoryReset() {
  if (!requireAdmin()) return;
  const char *namespaces[] = {config::kWifiNamespace, config::kPlaylistNamespace,
                              "player", "catalog", kSecurityNamespace, kUiNamespace};
  bool ok = true;
  for (const char *name : namespaces) { preferences.begin(name, false); ok = preferences.clear() && ok; preferences.end(); }
  if (!ok) { sendJson("{\"error\":\"could not clear all settings\"}", 500); return; }
  sendJson("{\"reset\":true,\"restarting\":true}");
  delay(300); ESP.restart();
}

void handleOtaUpload() {
  if (!isAdminRequest()) return;
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    otaSucceeded = false; otaInProgress = true; statusLedError = false; otaError = ""; addLog("ota", "firmware upload started");
    updateStatusLed(true);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) { otaError = Update.errorString(); otaInProgress = false; statusLedError = true; }
  } else if (upload.status == UPLOAD_FILE_WRITE && otaError.isEmpty()) {
    updateStatusLed();
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) { otaError = Update.errorString(); otaInProgress = false; statusLedError = true; }
  } else if (upload.status == UPLOAD_FILE_END && otaError.isEmpty()) {
    otaSucceeded = Update.end(true);
    if (!otaSucceeded) { otaError = Update.errorString(); otaInProgress = false; statusLedError = true; }
    addLog("ota", otaSucceeded ? "firmware verified" : otaError.c_str());
  } else if (upload.status == UPLOAD_FILE_ABORTED) { Update.end(); otaInProgress = false; statusLedError = true; otaError = "upload aborted"; addLog("ota", "upload aborted"); }
  if (upload.status != UPLOAD_FILE_WRITE) updateStatusLed(true);
}

void handleOtaResult() {
  if (!requireAdmin()) return;
  if (!otaSucceeded) { sendJson("{\"error\":\"" + jsonEscape(otaError.isEmpty() ? "OTA failed" : otaError) + "\"}", 500); return; }
  sendJson("{\"updated\":true,\"restarting\":true}"); delay(500); ESP.restart();
}

constexpr char kUserHtmlV10[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"><meta name="theme-color" content="#656b6a"><title>网络收音机</title><style>
:root{color-scheme:dark;--bg:#656b6a;--panel:#707675;--text:#fff;--muted:#d7dcda;--line:#858b89;--accent:#f2a51a}*{box-sizing:border-box}body{margin:0;background:#4e5453;color:var(--text);font-family:-apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif}.app{position:relative;width:100%;max-width:720px;min-height:100vh;margin:auto;padding:20px clamp(18px,5vw,42px) 50px;background:var(--bg);box-shadow:0 0 32px #0004}.settings{position:absolute;right:18px;top:16px;display:grid;place-items:center;width:48px;height:48px;border:0;border-radius:50%;background:#ffffff1c;color:#fff;text-decoration:none;font-size:27px}.settings:active{transform:scale(.96)}.hero{text-align:center;padding-top:58px}.cover-wrap{position:relative;width:min(48vw,250px);aspect-ratio:1;margin:auto;border-radius:22px;background:#f5f5f5;overflow:hidden;box-shadow:0 8px 25px #0003}.cover{width:100%;height:100%;object-fit:contain}.cover-fallback{position:absolute;inset:0;display:none;place-items:center;background:linear-gradient(145deg,#f5a623,#d47b13);font-size:clamp(46px,12vw,78px);font-weight:800}.station-name{min-height:1.5em;margin:25px 0 5px;font-size:clamp(25px,5vw,34px);font-weight:700}.state{color:var(--accent);font-size:18px}.progress{height:7px;margin:34px 0 28px;background:#a7adaa;border-radius:10px;overflow:hidden}.progress i{display:block;width:0;height:100%;background:var(--accent);transition:width .4s}.progress.busy i{width:58%;animation:load 1.5s ease-in-out infinite}@keyframes load{0%{transform:translateX(-110%)}100%{transform:translateX(180%)}}.transport{display:flex;align-items:center;justify-content:space-around;max-width:530px;margin:auto}.transport button{display:grid;place-items:center;border:0;color:#fff;background:transparent;cursor:pointer}.skip{width:80px;height:70px;font-size:42px}.play{width:108px;height:108px;border-radius:50%!important;background:#fff!important;color:#5e6463!important;font-size:48px;box-shadow:0 7px 22px #0003}.volume-row{display:flex;align-items:center;gap:13px;margin:30px 4px 24px;color:var(--muted)}input[type=range]{width:100%;accent-color:var(--accent)}.list-title{display:flex;align-items:center;justify-content:space-between;margin:15px 0 5px}.list-title h2{font-size:18px;margin:0}.count{color:var(--muted);font-size:14px}.station-list{border-top:1px solid var(--line)}.station{display:flex;align-items:center;gap:17px;width:100%;min-height:88px;padding:12px 10px;border:0;border-bottom:1px solid var(--line);background:transparent;color:#fff;text-align:left;cursor:pointer}.station.active{background:#ffffff12;border-left:4px solid var(--accent);padding-left:6px}.station img,.logo-fallback{flex:0 0 62px;width:62px;height:62px;border-radius:13px;background:#f7f7f7;object-fit:contain}.logo-fallback{display:grid;place-items:center;background:linear-gradient(145deg,#f5a623,#d47b13);color:#fff;font-size:25px;font-weight:800}.station b{font-size:19px;font-weight:600}.station small{display:block;margin-top:4px;color:var(--muted)}.empty,.error{padding:30px 8px;text-align:center;color:var(--muted)}@media(max-width:480px){.app{padding-left:16px;padding-right:16px}.hero{padding-top:50px}.cover-wrap{width:56vw}.station-name{font-size:25px}.play{width:94px;height:94px}.skip{font-size:34px}.station{min-height:78px}.station img,.logo-fallback{flex-basis:54px;width:54px;height:54px}}
</style></head><body><main class="app"><a class="settings" href="/admin" aria-label="进入管理页面" title="设置">⚙</a><section class="hero"><div class="cover-wrap"><img id="cover" class="cover" alt="当前电台台标"><div id="coverFallback" class="cover-fallback">R</div></div><div id="stationName" class="station-name">加载中…</div><div id="state" class="state">正在连接设备</div></section><div id="progress" class="progress"><i></i></div><nav class="transport" aria-label="播放控制"><button class="skip" onclick="stepStation(-1)" aria-label="上一台">◀</button><button id="play" class="play" onclick="togglePlay()" aria-label="播放或暂停">▶</button><button class="skip" onclick="stepStation(1)" aria-label="下一台">▶</button></nav><div class="volume-row"><span>🔉</span><input id="volume" type="range" min="0" max="21" aria-label="音量" oninput="queueVolume(this.value)"><span>🔊</span></div><div class="list-title"><h2>电台列表</h2><span id="count" class="count"></span></div><section id="stations" class="station-list"><div class="empty">正在加载电台…</div></section></main><script>
const textureStyles={none:['none','auto'],dots:['radial-gradient(#ffffff24 1px,transparent 1px)','18px 18px'],grid:['linear-gradient(#ffffff16 1px,transparent 1px),linear-gradient(90deg,#ffffff16 1px,transparent 1px)','24px 24px'],diagonal:['repeating-linear-gradient(135deg,#ffffff0d 0 2px,transparent 2px 12px)','auto'],cloud:['radial-gradient(circle at 12px 14px,transparent 9px,#ffffff1f 10px 11px,transparent 12px),radial-gradient(circle at 28px 14px,transparent 9px,#ffffff1f 10px 11px,transparent 12px)','40px 28px'],lattice:['linear-gradient(45deg,#ffffff14 12.5%,transparent 12.5% 37.5%,#ffffff14 37.5% 62.5%,transparent 62.5% 87.5%,#ffffff14 87.5%)','32px 32px'],waves:['radial-gradient(ellipse at 50% 100%,transparent 11px,#ffffff1c 12px 13px,transparent 14px)','34px 18px'],bamboo:['repeating-linear-gradient(90deg,transparent 0 30px,#ffffff16 31px 33px,transparent 34px 62px),repeating-linear-gradient(0deg,transparent 0 54px,#ffffff0d 55px 57px,transparent 58px 86px)','64px 88px'],ricepaper:['linear-gradient(25deg,#ffffff0a 1px,transparent 1px),linear-gradient(115deg,#ffffff08 1px,transparent 1px)','37px 53px,41px 47px'],porcelain:['radial-gradient(circle at 0 0,transparent 15px,#ffffff20 16px 17px,transparent 18px),radial-gradient(circle at 100% 100%,transparent 15px,#ffffff20 16px 17px,transparent 18px)','40px 40px']};fetch('/api/user/theme').then(r=>r.json()).then(t=>{document.documentElement.style.setProperty('--bg',t.background);document.documentElement.style.setProperty('--accent',t.accent);document.body.style.backgroundColor=t.background;const p=textureStyles[t.texture]||textureStyles.none,a=document.querySelector('.app');a.style.backgroundImage=p[0];a.style.backgroundSize=p[1];document.querySelector('meta[name="theme-color"]').content=t.background}).catch(()=>{});
const q=s=>document.querySelector(s);let stations=[],selected=-1,playerState='stopped',volumeTimer;async function api(url,options){const response=await fetch(url,options);const data=await response.json();if(!response.ok)throw Error(data.error||'操作失败');return data}function logoUrl(name){return name?'/logos/'+name.split('/').map(encodeURIComponent).join('/'):''}function setImage(img,fallback,station){fallback.textContent=(station.name||'R').trim().slice(0,1).toUpperCase();fallback.style.display='none';img.style.display='block';if(!station.logo){img.style.display='none';fallback.style.display='grid';return}img.onerror=()=>{img.style.display='none';fallback.style.display='grid'};img.src=logoUrl(station.logo)}function renderList(){const host=q('#stations');host.textContent='';q('#count').textContent=stations.length+' 个电台';if(!stations.length){host.innerHTML='<div class="empty">暂无电台，请到管理页面添加</div>';return}stations.forEach(s=>{const row=document.createElement('button');row.className='station'+(s.id===selected?' active':'');row.onclick=()=>selectStation(s.id);const img=document.createElement('img'),fallback=document.createElement('span');fallback.className='logo-fallback';setImage(img,fallback,s);const text=document.createElement('span'),name=document.createElement('b');name.textContent=s.name;text.append(name);if(s.id===selected){const hint=document.createElement('small');hint.textContent='当前电台';text.append(hint)}row.append(img,fallback,text);host.append(row)})}function renderNow(){const station=stations.find(s=>s.id===selected)||{name:'网络收音机',logo:''};q('#stationName').textContent=station.name;setImage(q('#cover'),q('#coverFallback'),station);q('#play').textContent=playerState==='playing'?'Ⅱ':'▶';const labels={playing:'正在播放',buffering_or_reconnecting:'正在缓冲',stopped:'已暂停'};q('#state').textContent=labels[playerState]||'正在恢复连接';q('#progress').classList.toggle('busy',playerState!=='playing'&&playerState!=='stopped')}async function loadStations(){const data=await api('/api/user/stations');stations=data.stations||[];selected=data.selected;renderList();renderNow()}async function refreshPlayer(){try{const data=await api('/api/user/player/status');playerState=data.state;selected=data.selected_station;q('#volume').value=data.volume;renderNow();document.querySelectorAll('.station').forEach((row,i)=>row.classList.toggle('active',stations[i]&&stations[i].id===selected))}catch(e){q('#state').textContent='设备连接失败'}}async function command(url){try{const data=await api(url,{method:'POST'});if(data.state!==undefined)playerState=data.state;if(data.selected_station!==undefined)selected=data.selected_station;await loadStations();renderNow()}catch(e){alert(e.message)}}function togglePlay(){command(playerState==='playing'?'/api/user/player/stop':'/api/user/player/play')}function selectStation(id){selected=id;playerState='buffering_or_reconnecting';renderList();renderNow();command('/api/user/stations/select?id='+id)}function stepStation(direction){if(!stations.length)return;const current=stations.findIndex(s=>s.id===selected),target=(current+(direction<0?-1:1)+stations.length)%stations.length;selectStation(stations[target].id)}function queueVolume(value){clearTimeout(volumeTimer);volumeTimer=setTimeout(()=>command('/api/user/player/volume?value='+value),180)}loadStations().then(refreshPlayer).catch(e=>q('#stations').innerHTML='<div class="error">'+e.message+'</div>');setInterval(refreshPlayer,2000);setInterval(loadStations,15000);
</script></body></html>
)HTML";

constexpr char kAdminHtmlV10[] PROGMEM =
R"HTML(<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>网络收音机 2.0 管理</title><style>:root{color-scheme:dark}body{max-width:880px;margin:24px auto;padding:0 16px;font:16px system-ui;background:#101827;color:#e5e7eb}section,pre,.station{background:#172234;padding:14px;border-radius:10px;margin:14px 0}button,input,select{box-sizing:border-box;padding:9px;margin:4px;border:0;border-radius:6px}input,select{width:100%}button{background:#38bdf8;color:#062032;font-weight:700}.warn{background:#fbbf24}.danger{background:#fb7185}.station img{width:48px;height:48px;object-fit:contain;background:#fff;border-radius:8px;vertical-align:middle;margin-right:10px}.station small{display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:#b7c6da}.active{outline:2px solid #38bdf8}.state{font-size:1.1em;color:#67e8f9;margin-bottom:24px}.transport{display:flex;align-items:center;justify-content:center;gap:clamp(28px,8vw,72px);margin:18px 0 28px}.transport button{display:grid;place-items:center;margin:0}.skip{width:76px;height:64px;border-radius:18px;font-size:25px;background:#263449;color:#dce6f5}.play-toggle{width:92px;height:92px;border-radius:50%;font-size:36px;background:#f8fafc;color:#172234;box-shadow:0 10px 28px #0005}.volume-head{display:flex;justify-content:space-between;align-items:center;margin:0 6px 8px;color:#cbd5e1}.volume-head b{color:#fff;font-size:1.15em}pre{overflow:auto}.volume{width:calc(100% - 10px)}a{color:#67e8f9}</style><h1>ESP32-S3 网络收音机</h1><p>版本号：)HTML"
NETWORK_RADIO_VERSION
R"HTML(　编译时间：)HTML"
__DATE__ " " __TIME__
R"HTML(　<a href="/">返回播放器</a></p>
<style>.station button{min-width:82px;padding:11px 17px}.theme-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}.theme-grid label{display:grid;gap:6px}.theme-grid input,.theme-grid select{margin:0}.theme-grid input[type=color]{width:100%;height:54px;padding:4px;border:1px solid #ffffff26;border-radius:10px;background:#fff;color-scheme:light;cursor:pointer}.theme-grid input[type=color]::-webkit-color-swatch-wrapper{padding:0}.theme-grid input[type=color]::-webkit-color-swatch{border:0;border-radius:6px}.theme-grid input[type=color]::-moz-color-swatch{border:0;border-radius:6px}@media(max-width:560px){.theme-grid{grid-template-columns:1fr}.station{overflow-x:auto;white-space:nowrap}.station small{white-space:normal}.station button{min-width:auto;padding:9px 11px;margin:3px 2px}}</style>
<section class="player"><h2>正在播放</h2><div id="now" class="state">读取中…</div><div class="transport"><button class="skip" aria-label="上一台" onclick="post('/api/player/previous')">◀◀</button><button id="playButton" class="play-toggle" aria-label="播放或暂停" onclick="togglePlayer()">▶</button><button class="skip" aria-label="下一台" onclick="post('/api/player/next')">▶▶</button></div><div class="volume-head"><span>音量</span><b><span id="vol">--</span>/21</b></div><input id="volume" class="volume" type="range" min="0" max="21" oninput="q('#vol').textContent=this.value" onchange="post('/api/player/volume?value='+this.value)"></section>
<section><h2>用户页面外观</h2><div class="theme-grid"><label>页面颜色<input id="uiBackground" type="color" value="#656b6a"></label><label>强调颜色<input id="uiAccent" type="color" value="#f2a51a"></label><label>纹理效果<select id="uiTexture"><option value="none">无纹理</option><option value="dots">圆点</option><option value="grid">网格</option><option value="diagonal">斜纹</option><option value="cloud">祥云</option><option value="lattice">回纹窗格</option><option value="waves">水波</option><option value="bamboo">竹影</option><option value="ricepaper">宣纸</option><option value="porcelain">青花</option></select></label></div><button onclick="saveUiTheme()">保存页面外观</button></section>
<section><h2>播放列表</h2><div id="stations">加载中…</div><h3 id="formTitle">新增电台</h3><input id="editId" type="hidden"><input id="name" placeholder="电台名称"><input id="url" placeholder="http(s):// 音频流地址"><button onclick="saveStation()">保存</button><button class="warn" onclick="clearForm()">取消编辑</button></section>
<section><h2>Wi-Fi</h2><button onclick="scanWifi()">扫描网络</button><select id="ssid"><option value="">选择 Wi-Fi</option></select><input id="pass" type="password" placeholder="Wi-Fi 密码"><button onclick="saveWifi()">保存并连接</button><button class="warn" onclick="forgetWifi()">清除 Wi-Fi 设置</button></section>
<section><h2>维护与安全</h2><button onclick="q('#firmware').click()">选择固件并升级</button><input id="firmware" type="file" accept=".bin" hidden onchange="ota(this.files[0])"><button onclick="downloadLog()">下载诊断日志</button><input id="adminPass" type="password" placeholder="设置管理密码（8–63 位，用户名 admin）"><button onclick="setPassword()">保存管理密码</button><button class="danger" onclick="factoryReset()">恢复出厂设置</button><p>配网热点密码独立：<code>radio-setup</code></p></section><pre id="status">读取中…</pre>
<script>let playerState='stopped';const q=s=>document.querySelector(s),enc=o=>new URLSearchParams(o);async function api(u,o){let r=await fetch(u,o),j=await r.json();if(!r.ok)throw Error(j.error||'请求失败');return j}async function loadUiTheme(){try{let t=await api('/api/ui-theme');q('#uiBackground').value=t.background;q('#uiAccent').value=t.accent;q('#uiTexture').value=t.texture}catch(e){}}async function saveUiTheme(){try{let body=enc({background:q('#uiBackground').value,accent:q('#uiAccent').value,texture:q('#uiTexture').value});await api('/api/ui-theme',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});alert('页面外观已保存')}catch(e){alert(e.message)}}async function post(u){try{await api(u,{method:'POST'});refresh()}catch(e){alert(e)}}async function togglePlayer(){await post(playerState==='playing'?'/api/player/stop':'/api/player/play')}async function refresh(){try{let[s,p,x]=await Promise.all([api('/api/status'),api('/api/stations'),api('/api/player/status')]);q('#status').textContent=JSON.stringify(s,null,2);q('#now').textContent=`${x.state} · ${s.player.selected_name} · ${x.message||''}`;playerState=x.state;q('#playButton').textContent=playerState==='playing'?'Ⅱ':'▶';q('#volume').value=x.volume;q('#vol').textContent=x.volume;render(p)}catch(e){q('#status').textContent='错误：'+e}}function render(p){q('#stations').innerHTML=p.stations.map(x=>`<div class="station ${x.id==p.selected?'active':''}">${x.logo?`<img src="/logos/${x.logo}" onerror="this.style.display='none'">`:``}<b>${x.name}</b><small>${x.url}</small><button onclick="post('/api/stations/select?id=${x.id}')">播放此台</button><button onclick="edit(${x.id})">编辑</button><button onclick="post('/api/stations/move?id=${x.id}&direction=first')">最前</button><button onclick="post('/api/stations/move?id=${x.id}&direction=up')">↑</button><button onclick="post('/api/stations/move?id=${x.id}&direction=down')">↓</button><button onclick="post('/api/stations/move?id=${x.id}&direction=last')">最后</button><button class="warn" onclick="removeStation(${x.id})">删除</button></div>`).join('')}async function saveStation(){let id=q('#editId').value,body=enc({name:q('#name').value,url:q('#url').value});try{await api(id===''?'/api/stations':'/api/stations/update?id='+id,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});clearForm();refresh()}catch(e){alert(e)}}async function edit(id){let p=await api('/api/stations'),x=p.stations[id];q('#editId').value=id;q('#name').value=x.name;q('#url').value=x.url;q('#formTitle').textContent='编辑电台'}function clearForm(){q('#editId').value='';q('#name').value='';q('#url').value='';q('#formTitle').textContent='新增电台'}function removeStation(id){if(confirm('删除该电台？'))post('/api/stations/delete?id='+id)}async function scanWifi(){let d=await api('/api/wifi/scan'),s=q('#ssid');s.innerHTML='<option value="">选择 Wi-Fi</option>';d.networks.forEach(n=>s.innerHTML+=`<option value="${n.ssid}">${n.ssid} (${n.rssi} dBm)</option>`)}async function saveWifi(){try{await api('/api/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc({ssid:q('#ssid').value,password:q('#pass').value})});q('#status').textContent='Wi-Fi 已保存，设备正在重启…'}catch(e){alert(e)}}async function forgetWifi(){if(confirm('清除保存的 Wi-Fi？'))await post('/api/wifi/forget')}async function setPassword(){try{await api('/api/security/password',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc({password:q('#adminPass').value})});alert('已启用管理密码；请刷新页面并用 admin 登录。')}catch(e){alert(e)}}async function ota(f){if(!f||!confirm('上传后设备会重启，继续？'))return;let d=new FormData;d.append('firmware',f);try{let r=await fetch('/api/ota',{method:'POST',body:d}),j=await r.json();if(!r.ok)throw Error(j.error);q('#status').textContent='升级完成，设备正在重启…'}catch(e){alert(e)}}function downloadLog(){location='/api/diagnostics/download'}async function factoryReset(){if(confirm('这将清除 Wi-Fi、电台、音量、页面外观和管理密码，确定？'))await post('/api/factory-reset')}loadUiTheme();refresh();setInterval(refresh,2000)</script></html>
)HTML";

void configureWebServerV8() {
  server.serveStatic("/logos/", LittleFS, "/logos/");
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html; charset=utf-8", kUserHtmlV10); });
  server.on("/admin", HTTP_GET, [] { if (requireAdmin()) server.send_P(200, "text/html; charset=utf-8", kAdminHtmlV10); });
  server.on("/api/user/stations", HTTP_GET, [] { sendJson(userPlaylistJson()); });
  server.on("/api/user/theme", HTTP_GET, [] { sendJson(uiThemeJson()); });
  server.on("/api/user/stations/select", HTTP_POST, handleSelectStation);
  server.on("/api/user/player/status", HTTP_GET, handlePlayerStatus);
  server.on("/api/user/player/play", HTTP_POST, handleUserPlay);
  server.on("/api/user/player/stop", HTTP_POST, handleUserStop);
  server.on("/api/user/player/volume", HTTP_POST, handlePlayerVolume);
  server.on("/api/user/player/previous", HTTP_POST, handleUserPrevious);
  server.on("/api/user/player/next", HTTP_POST, handleUserNext);
  server.on("/api/status", HTTP_GET, handleStatusV8);
  server.on("/api/stations", HTTP_GET, [] { if (requireAdmin()) sendJson(playlistJson()); });
  server.on("/api/stations", HTTP_POST, [] { if (requireAdmin()) handleAddStation(); });
  server.on("/api/stations/update", HTTP_POST, [] { if (requireAdmin()) handleUpdateStation(); });
  server.on("/api/stations/delete", HTTP_POST, [] { if (requireAdmin()) handleDeleteStation(); });
  server.on("/api/stations/select", HTTP_POST, [] { if (requireAdmin()) handleSelectStation(); });
  server.on("/api/stations/move", HTTP_POST, [] { if (requireAdmin()) handleMoveStationV11(); });
  server.on("/api/wifi/scan", HTTP_GET, [] { if (requireAdmin()) handleWifiScan(); });
  server.on("/api/wifi", HTTP_POST, [] { if (requireAdmin()) handleSaveWifi(); });
  server.on("/api/wifi/forget", HTTP_POST, [] { if (requireAdmin()) handleForgetWifi(); });
  server.on("/api/player/status", HTTP_GET, handlePlayerStatusV8);
  server.on("/api/player/play", HTTP_POST, handlePlayerPlayV8);
  server.on("/api/player/stop", HTTP_POST, handlePlayerStopV8);
  server.on("/api/player/volume", HTTP_POST, handlePlayerVolumeV8);
  server.on("/api/player/previous", HTTP_POST, handlePlayerPreviousV9);
  server.on("/api/player/next", HTTP_POST, handlePlayerNextV9);
  server.on("/api/diagnostics", HTTP_GET, handleDiagnostics);
  server.on("/api/diagnostics/download", HTTP_GET, handleDiagnosticsDownload);
  server.on("/api/security/password", HTTP_POST, handleSetPassword);
  server.on("/api/ui-theme", HTTP_GET, [] { if (requireAdmin()) sendJson(uiThemeJson()); });
  server.on("/api/ui-theme", HTTP_POST, handleSaveUiTheme);
  server.on("/api/factory-reset", HTTP_POST, handleFactoryReset);
  server.on("/api/ota", HTTP_POST, handleOtaResult, handleOtaUpload);
  server.onNotFound([] { server.sendHeader("Location", "/"); server.send(302, "text/plain", "Redirecting"); });
  server.begin();
}

}  // namespace

#ifndef NETWORK_RADIO_V8_NO_ENTRYPOINT
void setup() {
  Serial.begin(config::kSerialBaud); delay(300);
  rgbLedWrite(kStatusLedPin, 0, 0, 0);
  snprintf(accessPointSsid, sizeof(accessPointSsid), "%s%04X", config::kAccessPointPrefix, setupAccessPointId());
  loadPlaylist();
  migrateStationCatalog();
  importBuiltinStations();
  const bool newsStationsAdded = importNewsStationPack();
  enrichStationIcons();
  sortStationsByRegionOnce(newsStationsAdded);
  loadSecurity(); loadUiTheme();
  const bool iconsMounted = LittleFS.begin(false);
  Serial.printf("LittleFS icons: %s\n", iconsMounted ? "mounted" : "mount failed");
  playerPreferences.begin("player", true); playerVolume = playerPreferences.getUChar("volume", playerVolume); playerPreferences.end();
  Audio::audio_info_callback = audioInfoV8;
  audio.settings.BUFFER_TRESHOLD_HLS = 32 * 1024;
  audio.setPinout(config::kI2sBclk, config::kI2sLrclk, config::kI2sDataOut); audio.setVolume(playerVolume);
  const bool connected = connectSavedStation();
  if (!connected || config::kKeepSetupAccessPointAvailable) startAccessPoint();
  wasStationConnected = connected;
  onStationSelected = onPlaylistSelectionV8;
  configureWebServerV8();
  addLog("boot", config::kFirmwareVersion);
  if (connected) startSelectedStationV8("boot playback");
}

void loop() {
  if (accessPointRunning) dnsServer.processNextRequest();
  server.handleClient();
  maintainNetworkAndPlayback();
  audio.loop();
  updateStatusLed();
}
#endif  // NETWORK_RADIO_V8_NO_ENTRYPOINT
