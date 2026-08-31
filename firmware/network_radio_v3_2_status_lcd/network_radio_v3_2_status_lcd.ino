/*
 * Network Radio 3.2.0 standalone Arduino sketch with ST7735R status LCD.
 * Project-local source dependencies are inlined in this file.
 *
 * The superseded V4 test-tone/I2S path and unused legacy web pages have
 * been removed. The active player, administration UI, OTA, recovery,
 * playlist, and WS2812 status paths are retained.
 */

/* Network Radio 3.0: player UI, administration UI, and WS2812B status LED. */

#define NETWORK_RADIO_VERSION "3.2.0-status-lcd"
#define NETWORK_RADIO_MAX_STATIONS 140
#ifdef NETWORK_RADIO_NO_ENTRYPOINT
#define NETWORK_RADIO_V8_NO_ENTRYPOINT
#endif

// BEGIN INLINED: firmware/network_radio_v5_playback/network_radio_v5_playback.ino
/* Network Radio V5: V4 playlist management plus HLS/AAC playback. */

#include <Audio.h>
#include <LittleFS.h>
#include <esp32-hal-psram.h>

// Reuse V4's provisioning and playlist persistence helpers.
#ifndef NETWORK_RADIO_VERSION
#define NETWORK_RADIO_VERSION "0.5.0-playback"
#endif
// BEGIN INLINED: firmware/network_radio_v4_playlist/network_radio_v4_playlist.ino
/*
 * Network Radio V4 - Wi-Fi provisioning + persistent playlist management.
 * Hardware output: ESP32-S3 GPIO4/5/6 -> MAX98357A BCLK/LRCLK/DIN.
 */

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <SPI.h>
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
constexpr uint8_t kTftCs = 10;
constexpr uint8_t kTftMosi = 11;
constexpr uint8_t kTftSclk = 12;
constexpr uint8_t kTftBacklight = 13;
constexpr uint8_t kTftDc = 14;
constexpr uint8_t kTftReset = 15;
constexpr uint32_t kTftSpiHz = 1000000;

constexpr char kWifiNamespace[] = "radio";
constexpr char kWifiSsidKey[] = "wifi_ssid";
constexpr char kWifiPasswordKey[] = "wifi_pass";
constexpr char kPlaylistNamespace[] = "playlist";
constexpr char kPlaylistCountKey[] = "count";
constexpr char kPlaylistSelectedKey[] = "selected";
constexpr char kPlaylistSequenceKey[] = "sequence";
constexpr char kPlaylistBackendKey[] = "backend";
constexpr uint8_t kPlaylistBackendLegacyNvs = 1;
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
Station *stations = nullptr;
uint8_t stationCount = 0;
uint8_t selectedStation = 0;
bool playlistStorageReady = false;
bool playlistFileStoreUnavailable = false;
bool legacyPlaylistMigrationPending = false;
uint8_t playlistActiveSlot = 0;
uint32_t playlistSequence = 0;
uint32_t playlistRevision = 1;

char accessPointSsid[20] = {};
bool accessPointRunning = false;
bool stationConfigured = false;
bool wifiCredentialsPresent = false;
bool wifiScanInProgress = false;
bool mdnsRunning = false;
void (*onStationSelected)(uint8_t) = nullptr;

bool requireAdmin();

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

constexpr char kPlaylistSlotA[] = "/playlist_a.bin";
constexpr char kPlaylistSlotB[] = "/playlist_b.bin";
constexpr uint32_t kPlaylistMagic = 0x3150524EU;  // "NRP1"
constexpr uint16_t kPlaylistFormatVersion = 1;

struct PlaylistFileHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  uint8_t selected;
  uint8_t reserved[3];
  uint32_t sequence;
  uint32_t checksum;
};

static_assert(sizeof(Station) == config::kStationNameSize +
                                  config::kStationUrlSize +
                                  config::kStationLogoSize,
              "Station must remain a packed character record for playlist storage");

bool allocateStationStore() {
  if (stations != nullptr) return true;
  if (!psramFound() && !psramInit()) {
    Serial.println("ERROR: PSRAM is required for the station store.");
    return false;
  }
  stations = static_cast<Station *>(
      ps_calloc(config::kMaxStations, sizeof(Station)));
  if (stations == nullptr) {
    Serial.println("ERROR: Could not allocate the station store in PSRAM.");
    return false;
  }
  return true;
}

uint32_t playlistChecksumUpdate(uint32_t value, const void *data, size_t length) {
  const uint8_t *bytes = static_cast<const uint8_t *>(data);
  for (size_t index = 0; index < length; ++index) {
    value ^= bytes[index];
    value *= 16777619UL;
  }
  return value;
}

uint32_t playlistChecksum(uint16_t count, uint8_t selected,
                          const Station *entries) {
  uint32_t value = 2166136261UL;
  value = playlistChecksumUpdate(value, &count, sizeof(count));
  value = playlistChecksumUpdate(value, &selected, sizeof(selected));
  return playlistChecksumUpdate(value, entries, count * sizeof(Station));
}

bool sequenceIsNewer(uint32_t candidate, uint32_t reference) {
  return static_cast<int32_t>(candidate - reference) > 0;
}

void markPlaylistChanged() {
  ++playlistRevision;
  if (playlistRevision == 0) ++playlistRevision;
}

bool inspectPlaylistFile(const char *path, PlaylistFileHeader &header) {
  File file = LittleFS.open(path, "r");
  if (!file) return false;
  const bool headerRead = file.read(reinterpret_cast<uint8_t *>(&header),
                                    sizeof(header)) == sizeof(header);
  if (!headerRead || header.magic != kPlaylistMagic ||
      header.version != kPlaylistFormatVersion || header.count == 0 ||
      header.count > config::kMaxStations || header.selected >= header.count ||
      file.size() != sizeof(header) + header.count * sizeof(Station)) {
    file.close();
    return false;
  }

  uint32_t checksum = 2166136261UL;
  checksum = playlistChecksumUpdate(checksum, &header.count, sizeof(header.count));
  checksum = playlistChecksumUpdate(checksum, &header.selected,
                                    sizeof(header.selected));
  Station scratch;
  for (uint16_t index = 0; index < header.count; ++index) {
    if (file.read(reinterpret_cast<uint8_t *>(&scratch), sizeof(scratch)) !=
        sizeof(scratch)) {
      file.close();
      return false;
    }
    checksum = playlistChecksumUpdate(checksum, &scratch, sizeof(scratch));
  }
  file.close();
  return checksum == header.checksum;
}

bool readPlaylistFile(const char *path, const PlaylistFileHeader &expected) {
  File file = LittleFS.open(path, "r");
  PlaylistFileHeader header = {};
  if (!file || file.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) !=
                   sizeof(header) ||
      header.sequence != expected.sequence || header.count != expected.count ||
      header.checksum != expected.checksum) {
    if (file) file.close();
    return false;
  }
  for (uint16_t index = 0; index < header.count; ++index) {
    if (file.read(reinterpret_cast<uint8_t *>(&stations[index]),
                  sizeof(Station)) != sizeof(Station)) {
      file.close();
      return false;
    }
  }
  file.close();
  stationCount = static_cast<uint8_t>(header.count);
  selectedStation = header.selected;
  playlistSequence = header.sequence;
  return true;
}

bool loadPlaylistFromFiles() {
  if (!playlistStorageReady) return false;
  PlaylistFileHeader headerA = {};
  PlaylistFileHeader headerB = {};
  const bool validA = inspectPlaylistFile(kPlaylistSlotA, headerA);
  const bool validB = inspectPlaylistFile(kPlaylistSlotB, headerB);
  if (!validA && !validB) return false;

  const bool useA = validA && (!validB || !sequenceIsNewer(headerB.sequence,
                                                              headerA.sequence));
  if (!readPlaylistFile(useA ? kPlaylistSlotA : kPlaylistSlotB,
                        useA ? headerA : headerB)) {
    return false;
  }
  playlistActiveSlot = useA ? 0 : 1;

  if (preferences.begin(config::kPlaylistNamespace, true)) {
    const uint32_t storedSequence = preferences.getUInt(
        config::kPlaylistSequenceKey, 0);
    const uint8_t storedSelection = preferences.getUChar(
        config::kPlaylistSelectedKey, selectedStation);
    // If a first migration was interrupted after one snapshot, retain the
    // legacy NVS copy until a later structural save has restored both slots.
    legacyPlaylistMigrationPending =
        preferences.getUChar(config::kPlaylistCountKey, 0) > 0;
    preferences.end();
    if (storedSequence == playlistSequence && storedSelection < stationCount) {
      selectedStation = storedSelection;
    }
  }
  return true;
}

void logPlaylistFileWriteFailure(const char *path, const char *stage) {
  Serial.printf("WARN: Playlist snapshot %s %s (LittleFS %u/%u bytes).\n",
                path, stage, static_cast<unsigned>(LittleFS.usedBytes()),
                static_cast<unsigned>(LittleFS.totalBytes()));
}

bool writePlaylistFile(const char *path, uint32_t sequence) {
  PlaylistFileHeader header = {
      kPlaylistMagic,
      kPlaylistFormatVersion,
      stationCount,
      selectedStation,
      {0, 0, 0},
      sequence,
      playlistChecksum(stationCount, selectedStation, stations),
  };
  File file = LittleFS.open(path, "w");
  if (!file) {
    logPlaylistFileWriteFailure(path, "could not be opened for writing");
    return false;
  }
  if (file.write(reinterpret_cast<const uint8_t *>(&header), sizeof(header)) !=
      sizeof(header)) {
    file.close();
    logPlaylistFileWriteFailure(path, "header write failed");
    return false;
  }
  for (uint8_t index = 0; index < stationCount; ++index) {
    if (file.write(reinterpret_cast<const uint8_t *>(&stations[index]),
                   sizeof(Station)) == sizeof(Station)) {
      continue;
    }
    file.close();
    Serial.printf(
        "WARN: Playlist snapshot %s data write failed at record %u "
        "(LittleFS %u/%u bytes).\n",
        path, static_cast<unsigned>(index),
        static_cast<unsigned>(LittleFS.usedBytes()),
        static_cast<unsigned>(LittleFS.totalBytes()));
    return false;
  }
  file.flush();
  file.close();
  PlaylistFileHeader verified = {};
  const bool verifiedOk = inspectPlaylistFile(path, verified) &&
                          verified.sequence == sequence &&
                          verified.count == stationCount &&
                          verified.selected == selectedStation;
  if (!verifiedOk) {
    logPlaylistFileWriteFailure(path, "verification failed");
  }
  return verifiedOk;
}

bool bothPlaylistSlotsAreValid() {
  PlaylistFileHeader headerA = {};
  PlaylistFileHeader headerB = {};
  return inspectPlaylistFile(kPlaylistSlotA, headerA) &&
         inspectPlaylistFile(kPlaylistSlotB, headerB);
}

bool initialisePlaylistSlots() {
  // A first-run or NVS migration gets two independently verified snapshots
  // before the old data is retired.  A power cut can therefore leave at most
  // one valid new file, never an empty playlist with no recovery source.
  const uint32_t firstSequence = playlistSequence == 0 ? 1 : playlistSequence + 1;
  if (!writePlaylistFile(kPlaylistSlotA, firstSequence)) return false;
  const uint32_t secondSequence = firstSequence + 1;
  if (!writePlaylistFile(kPlaylistSlotB, secondSequence)) return false;
  playlistActiveSlot = 1;
  playlistSequence = secondSequence;
  return true;
}

bool retireLegacyPlaylist() {
  if (!legacyPlaylistMigrationPending || !bothPlaylistSlotsAreValid()) return true;
  if (!preferences.begin(config::kPlaylistNamespace, false)) return false;
  const bool saved = preferences.clear() &&
                     preferences.putUChar(config::kPlaylistSelectedKey,
                                           selectedStation) == 1 &&
                     preferences.putUInt(config::kPlaylistSequenceKey,
                                         playlistSequence) == sizeof(uint32_t);
  preferences.end();
  if (saved) legacyPlaylistMigrationPending = false;
  return saved;
}

bool persistLegacyPlaylist() {
  if (!preferences.begin(config::kPlaylistNamespace, false)) return false;
  bool saved = preferences.clear() &&
               preferences.putUChar(config::kPlaylistCountKey, stationCount) == 1 &&
               preferences.putUChar(config::kPlaylistSelectedKey, selectedStation) == 1;
  for (uint8_t index = 0; saved && index < stationCount; ++index) {
    const String entry = String(stations[index].name) + '\n' +
                         stations[index].url + '\n' + stations[index].logo;
    saved = preferences.putString(stationKey(index).c_str(), entry) ==
            entry.length();
  }
  if (saved) {
    saved = preferences.putUChar(config::kPlaylistBackendKey,
                                 config::kPlaylistBackendLegacyNvs) == 1;
  }
  preferences.end();
  if (saved) legacyPlaylistMigrationPending = true;
  return saved;
}

bool playlistPrefersLegacyStore() {
  if (!preferences.begin(config::kPlaylistNamespace, true)) return false;
  const bool preferred = preferences.getUChar(config::kPlaylistBackendKey, 0) ==
                         config::kPlaylistBackendLegacyNvs;
  preferences.end();
  return preferred;
}

bool markLegacyPlaylistStorePreferred() {
  if (!preferences.begin(config::kPlaylistNamespace, false)) return false;
  const bool saved = preferences.putUChar(config::kPlaylistBackendKey,
                                           config::kPlaylistBackendLegacyNvs) == 1;
  preferences.end();
  return saved;
}

bool persistPlaylistState() {
  if (!preferences.begin(config::kPlaylistNamespace, false)) return false;
  const bool saved = preferences.putUChar(config::kPlaylistSelectedKey,
                                           selectedStation) == 1 &&
                     preferences.putUInt(config::kPlaylistSequenceKey,
                                         playlistSequence) == sizeof(uint32_t);
  preferences.end();
  return saved;
}

bool persistSelectedStation() {
  // Selecting a station writes only the tiny state record.  The two-slot
  // playlist snapshot is reserved for structural changes, avoiding a full
  // 48 KiB rewrite for every tap on the player UI.
  return persistPlaylistState();
}

void setDefaultPlaylist() {
  stationCount = 1;
  selectedStation = 0;
  stations[0] = Station{};
  strlcpy(stations[0].name, "凤凰卫视音频", sizeof(stations[0].name));
  strlcpy(stations[0].url,
          "http://playtv-live.ifeng.com/live/06OLEEWQKN4_audio.m3u8",
          sizeof(stations[0].url));
}

bool persistPlaylist(bool preserveLoadedLegacy = false) {
  bool saved = false;
  if (!playlistStorageReady) {
    playlistFileStoreUnavailable = true;
  } else if (!playlistFileStoreUnavailable) {
    PlaylistFileHeader headerA = {};
    PlaylistFileHeader headerB = {};
    const bool validA = inspectPlaylistFile(kPlaylistSlotA, headerA);
    const bool validB = inspectPlaylistFile(kPlaylistSlotB, headerB);
    if (!validA && !validB) {
      saved = initialisePlaylistSlots();
    } else {
      const bool useA = validA &&
                        (!validB || !sequenceIsNewer(headerB.sequence,
                                                      headerA.sequence));
      playlistActiveSlot = useA ? 0 : 1;
      playlistSequence = useA ? headerA.sequence : headerB.sequence;
      const uint8_t nextSlot = playlistActiveSlot == 0 ? 1 : 0;
      const char *path = nextSlot == 0 ? kPlaylistSlotA : kPlaylistSlotB;
      const uint32_t nextSequence = playlistSequence + 1;
      saved = writePlaylistFile(path, nextSequence);
      if (saved) {
        playlistActiveSlot = nextSlot;
        playlistSequence = nextSequence;
      }
    }
    if (saved) {
      if (!retireLegacyPlaylist()) {
        Serial.println("WARN: Legacy playlist has been kept as a recovery copy.");
      } else if (!persistPlaylistState()) {
        Serial.println("WARN: Could not persist playlist selection state.");
      }
    }
    if (!saved) {
      playlistFileStoreUnavailable = true;
      Serial.println(
          "WARN: LittleFS playlist snapshots are unavailable; subsequent "
          "playlist changes will use legacy NVS storage.");
    }
  }
  if (!saved) {
    if (preserveLoadedLegacy) {
      if (!markLegacyPlaylistStorePreferred()) {
        Serial.println("WARN: Could not mark the legacy playlist as preferred.");
      }
      return false;
    }
    saved = persistLegacyPlaylist();
  }
  if (saved) markPlaylistChanged();
  return saved;
}

bool loadLegacyPlaylist() {
  stationCount = 0;
  selectedStation = 0;
  memset(stations, 0, config::kMaxStations * sizeof(Station));
  if (!preferences.begin(config::kPlaylistNamespace, true)) {
    return false;
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
  if (selectedStation >= stationCount) {
    selectedStation = 0;
  }
  legacyPlaylistMigrationPending = stationCount > 0;
  return stationCount > 0;
}

void loadPlaylist() {
  const bool preferLegacy = playlistPrefersLegacyStore();
  if (!preferLegacy && loadPlaylistFromFiles()) return;

  if (loadLegacyPlaylist()) {
    if (preferLegacy) {
      playlistFileStoreUnavailable = true;
      Serial.println(
          "WARN: Using the preserved legacy NVS playlist after a prior "
          "LittleFS write failure.");
      return;
    }
    if (!persistPlaylist(true)) {
      Serial.println(
          "WARN: LittleFS playlist storage unavailable; legacy NVS playlist "
          "retained.");
    }
    return;
  }

  if (preferLegacy && loadPlaylistFromFiles()) {
    Serial.println("WARN: Legacy NVS playlist unavailable; recovered from LittleFS.");
    return;
  }

  setDefaultPlaylist();
  if (!persistPlaylist()) {
    Serial.println("ERROR: Could not persist the playlist.");
  }
}

String playlistJson() {
  String json;
  json.reserve(48 + stationCount * 420U);
  json = "{\"revision\":" + String(playlistRevision) +
         ",\"selected\":" + String(selectedStation) + ",\"stations\":[";
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
  if (!preferences.begin(config::kWifiNamespace, true)) {
    Serial.println("ERROR: Could not read saved Wi-Fi credentials.");
    return false;
  }
  const String ssid = preferences.getString(config::kWifiSsidKey, "");
  const String password = preferences.getString(config::kWifiPasswordKey, "");
  preferences.end();
  wifiCredentialsPresent = !ssid.isEmpty();
  // A saved network and a live association are distinct states.  Retain the
  // former after a failed boot-time association so AP+STA recovery can retry
  // without discarding the STA interface.
  stationConfigured = wifiCredentialsPresent;
  if (ssid.isEmpty()) {
    return false;
  }
  WiFi.mode(accessPointRunning ? WIFI_AP_STA : WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), password.c_str());
  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startedAt < config::kStationConnectTimeoutMs) {
    delay(250);
  }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(false, false);
    return false;
  }
  stationConfigured = true;
  startMdns();
  return true;
}

void handleWifiScan() {
  if (!wifiScanInProgress) {
    // Scanning requires the station interface.  A device without saved
    // credentials normally runs in AP-only mode, so preserve the setup AP
    // while enabling STA before starting the asynchronous scan.
    if (accessPointRunning && WiFi.getMode() == WIFI_AP) {
      WiFi.mode(WIFI_AP_STA);
    }
    WiFi.scanDelete();
    const int result = WiFi.scanNetworks(true, true);
    if (result == WIFI_SCAN_FAILED) {
      Serial.printf("ERROR: Wi-Fi scan could not start (mode=%d, status=%d).\n",
                    static_cast<int>(WiFi.getMode()), static_cast<int>(WiFi.status()));
      sendJson("{\"error\":\"Wi-Fi scan could not start\"}", 503);
      return;
    }
    wifiScanInProgress = true;
    Serial.println("Wi-Fi scan started.");
    sendJson("{\"scanning\":true}", 202);
    return;
  }
  const int count = WiFi.scanComplete();
  if (count == WIFI_SCAN_RUNNING) {
    sendJson("{\"scanning\":true}", 202);
    return;
  }
  wifiScanInProgress = false;
  if (count < 0) {
    Serial.printf("ERROR: Wi-Fi scan failed (result=%d, mode=%d, status=%d).\n",
                  count, static_cast<int>(WiFi.getMode()), static_cast<int>(WiFi.status()));
    WiFi.scanDelete();
    sendJson("{\"error\":\"Wi-Fi scan failed\"}", 500);
    return;
  }
  Serial.printf("Wi-Fi scan completed: %d network(s).\n", count);
  String json = "{\"networks\":[";
  json.reserve(32 + static_cast<size_t>(count) * 64U);
  for (int index = 0; index < count; ++index) {
    if (index) json += ',';
    json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(index)) + "\",\"rssi\":" +
            String(WiFi.RSSI(index)) + "}";
  }
  WiFi.scanDelete();
  sendJson(json + "]}");
}

void handleSaveWifi() {
  if (!requireAdmin()) return;
  const String ssid = server.arg("ssid");
  const String password = server.arg("password");
  if (ssid.isEmpty() || ssid.length() > 32 ||
      (!password.isEmpty() && (password.length() < 8 || password.length() > 63))) {
    sendJson("{\"error\":\"invalid SSID or password\"}", 400);
    return;
  }
  if (!preferences.begin(config::kWifiNamespace, false)) {
    sendJson("{\"error\":\"could not open Wi-Fi settings\"}", 500);
    return;
  }
  const bool wroteSsid = preferences.putString(config::kWifiSsidKey, ssid) == ssid.length();
  preferences.putString(config::kWifiPasswordKey, password);
  const bool wrotePassword = preferences.getString(config::kWifiPasswordKey, "\x01") == password;
  const bool saved = wroteSsid && wrotePassword;
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
  if (!requireAdmin()) return;
  if (!preferences.begin(config::kWifiNamespace, false)) {
    sendJson("{\"error\":\"could not open Wi-Fi settings\"}", 500);
    return;
  }
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
  const String value = server.arg("id");
  if (value.isEmpty()) return false;
  uint16_t parsed = 0;
  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value[index];
    if (character < '0' || character > '9') return false;
    parsed = parsed * 10U + static_cast<uint8_t>(character - '0');
    if (parsed >= stationCount) return false;
  }
  id = static_cast<uint8_t>(parsed);
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
  stations[stationCount] = Station{};
  strlcpy(stations[stationCount].name, name.c_str(), sizeof(stations[0].name));
  strlcpy(stations[stationCount].url, url.c_str(), sizeof(stations[0].url));
  ++stationCount;
  if (!persistPlaylist()) {
    --stationCount;
    stations[stationCount] = Station{};
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
  const Station previous = stations[id];
  strlcpy(stations[id].name, name.c_str(), sizeof(stations[id].name));
  strlcpy(stations[id].url, url.c_str(), sizeof(stations[id].url));
  const bool saved = persistPlaylist();
  if (!saved) stations[id] = previous;
  sendJson(saved ? playlistJson() : "{\"error\":\"could not save playlist\"}",
           saved ? 200 : 500);
}

void handleDeleteStation() {
  uint8_t id;
  if (!parseStationId(id) || stationCount <= 1) {
    sendJson("{\"error\":\"cannot delete this station\"}", 400);
    return;
  }
  const uint8_t previousSelection = selectedStation;
  const Station removed = stations[id];
  for (uint8_t index = id; index + 1 < stationCount; ++index) {
    stations[index] = stations[index + 1];
  }
  --stationCount;
  stations[stationCount] = Station{};
  if (id < selectedStation) {
    --selectedStation;
  } else if (id == selectedStation && selectedStation >= stationCount) {
    selectedStation = stationCount - 1;
  }
  const bool saved = persistPlaylist();
  if (!saved) {
    for (uint8_t index = stationCount; index > id; --index) {
      stations[index] = stations[index - 1];
    }
    stations[id] = removed;
    ++stationCount;
    selectedStation = previousSelection;
  }
  sendJson(saved ? playlistJson() : "{\"error\":\"could not save playlist\"}",
           saved ? 200 : 500);
}

void handleSelectStation() {
  uint8_t id;
  if (!parseStationId(id)) {
    sendJson("{\"error\":\"invalid station id\"}", 400);
    return;
  }
  const uint8_t previousSelection = selectedStation;
  selectedStation = id;
  const bool saved = persistSelectedStation();
  if (!saved) selectedStation = previousSelection;
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

}  // namespace
// END INLINED: firmware/network_radio_v4_playlist/network_radio_v4_playlist.ino

namespace {

Audio audio;
Preferences playerPreferences;
uint8_t playerVolume = 12;  // ESP32-audioI2S range: 0..21
char playerMessage[96] = "idle";
bool playerRequested = false;

void setPlayerMessage(const char *message) {
  strlcpy(playerMessage, message ? message : "", sizeof(playerMessage));
}

void audioInfo(Audio::msg_t message) {
  if (message.msg != nullptr) setPlayerMessage(message.msg);
  if (message.s != nullptr && message.msg != nullptr) {
    Serial.printf("audio %s: %s\n", message.s, message.msg);
  }
}

bool startSelectedStation() {
  if (stationCount == 0 || WiFi.status() != WL_CONNECTED) {
    playerRequested = false;
    setPlayerMessage("waiting for router Wi-Fi");
    return false;
  }
  audio.stopSong();
  playerRequested = audio.connecttohost(stations[selectedStation].url);
  setPlayerMessage(playerRequested ? "connecting" : "connection failed");
  return playerRequested;
}

void onPlaylistSelection(uint8_t) {
  startSelectedStation();
}

void handlePlayerStatus() {
  const char *state = audio.isRunning() ? "playing" :
                      (playerRequested ? "buffering_or_reconnecting" : "stopped");
  String json = "{\"state\":\"" + String(state) + "\",\"volume\":" +
                String(playerVolume) + ",\"selected_station\":" +
                String(selectedStation) + ",\"playlist_revision\":" +
                String(playlistRevision) + ",\"input_buffer_bytes\":" +
                String(audio.inBufferFilled()) + ",\"sample_rate_hz\":" +
                String(audio.getSampleRate()) + ",\"bitrate\":" +
                String(audio.getBitRate()) + ",\"message\":\"" +
                jsonEscape(playerMessage) + "\"}";
  sendJson(json);
}

void handlePlayerPlay() {
  if (!startSelectedStation()) {
    sendJson("{\"error\":\"router Wi-Fi is not connected\"}", 503);
    return;
  }
  handlePlayerStatus();
}

void handlePlayerStop() {
  audio.stopSong();
  playerRequested = false;
  setPlayerMessage("stopped");
  handlePlayerStatus();
}

void handlePlayerVolume() {
  const int volume = server.arg("value").toInt();
  if (volume < 0 || volume > 21) {
    sendJson("{\"error\":\"volume must be 0..21\"}", 400);
    return;
  }
  playerVolume = static_cast<uint8_t>(volume);
  audio.setVolume(playerVolume);
  playerPreferences.begin("player", false);
  playerPreferences.putUChar("volume", playerVolume);
  playerPreferences.end();
  handlePlayerStatus();
}

void registerPlayerRoutes() {
  server.on("/api/player/status", HTTP_GET, handlePlayerStatus);
  server.on("/api/player/play", HTTP_POST, handlePlayerPlay);
  server.on("/api/player/stop", HTTP_POST, handlePlayerStop);
  server.on("/api/player/volume", HTTP_POST, handlePlayerVolume);
}

}  // namespace

// END INLINED: firmware/network_radio_v5_playback/network_radio_v5_playback.ino


// BEGIN INLINED: firmware/network_radio_v1_1_ws2812_status/builtin_stations.h
struct BuiltinStation { const char *name; const char *url; };
constexpr BuiltinStation kBuiltinStations[] = {
  {"CNR中国之声","https://ngcdn001.cnr.cn/live/zgzs/index.m3u8"},
  {"CNR经济之声","https://ngcdn002.cnr.cn/live/jjzs/index.m3u8"},
  {"CNR环球资讯","https://radio.0472.org/?id=692"},
  {"CNR音乐之声","https://radio.0472.org/?id=641"},
  {"CNR交通广播","https://radio.0472.org/?id=653"},
  {"CNR文艺之声","https://radio.0472.org/?id=648"},
  {"CNR湾区之声","https://radio.0472.org/?id=645"},
  {"CNR经典音乐","https://radio.0472.org/?id=642"},
  {"CNR台海之声","https://radio.0472.org/?id=643"},
  {"CNR神州之声","https://radio.0472.org/?id=644"},
  {"CNR香港之声","https://radio.0472.org/?id=646"},
  {"CNR民族之声","https://radio.0472.org/?id=647"},
  {"CNR老年之声","https://radio.0472.org/?id=649"},
  {"CNR乡村之声","https://radio.0472.org/?id=654"},
  {"CNR南海之声","https://radio.0472.org/?id=664"},
  {"北京交通广播","http://ls.qingting.fm/live/336.m3u8"},
  {"北京新闻广播","https://lhttp.qtfm.cn/live/339/64k.mp3"},
  {"北京文艺广播","http://ls.qingting.fm/live/333.m3u8"},
  {"北京城市广播","https://brtv-radiolive.rbc.cn/alive/fm1073.m3u8"},
  {"北京体育广播","https://brtv-radiolive.rbc.cn/alive/fm1025.m3u8"},
  {"北京阳光调频","https://lhttp.qtfm.cn/live/5021739/64k.mp3"},
  {"北京经典调频","https://radio.0472.org/?id=1254"},
  {"CRI华语环球","http://sk.cri.cn/hyhq.m3u8"},
  {"CRI环球资讯","https://sk.cri.cn/905.m3u8"},
  {"CRI南海之声","https://sk.cri.cn/nhzs.m3u8"},
  {"CRI英语资讯","http://sk.cri.cn/am846.m3u8"},
  {"RTHK3","https://rthkradio3-live.akamaized.net/hls/live/2040079/radio3/master.m3u8"},
  {"香港电台普通话台","https://rthkradiopth-live.akamaized.net/hls/live/2040082/radiopth/master.m3u8"},
  {"湖南经济广播","https://radio.0472.org/?id=1056"},
  {"湖南新闻频道","https://radio.0472.org/?id=525"},
  {"湖南潇湘之声","https://radio.0472.org/?id=526"},
  {"重庆文艺广播","http://satellitepull.cnr.cn/live/wxcqwygb/playlist.m3u8"},
  {"上海故事广播","http://live.cooltv.top/tv/news1296.php?id=10"},
  {"山西文艺广播","http://radiolive.sxrtv.com/live/wenyi/playlist.m3u8"},
  {"江苏文艺广播","http://satellitepull.cnr.cn/live/wx32jswygb/playlist.m3u8"},
  {"江苏故事广播","http://satellitepull.cnr.cn/live/wx32jsgsgb/playlist.m3u8"},
  {"上海戏曲广播","https://radio.0472.org/?id=1314"},
  {"陕西故事广播","https://radio.0472.org/?id=1133"},
  {"北京音乐广播","http://ls.qingting.fm/live/332.m3u8"},
  {"湖南金鹰之声","https://radio.0472.org/?id=523"},
  {"芒果时空音乐","https://radio.0472.org/?id=524"},
  {"湖南交通广播","https://radio.0472.org/?id=1059"},
  {"湖南音乐之声","https://radio.0472.org/?id=1060"},
  {"长沙交通广播","https://radio.0472.org/?id=1061"},
  {"长沙音乐广播","https://radio.0472.org/?id=1531"},
  {"广东音乐之声","http://ls.qingting.fm/live/1260.m3u8"},
  {"江西音乐广播","http://satellitepull.cnr.cn/live/wx32jiangxyygb/playlist.m3u8"},
  {"河北音乐广播","https://radio.0472.org/?id=381"},
  {"深圳音乐频率","https://radio.0472.org/?id=498"},
  {"南京音乐广播","http://hls.njgb.com/live_hls/4/playlist.m3u8"},
  {"江苏音乐广播","https://radio.0472.org/?id=415"},
  {"河北汽车音乐","https://radio.pull.hebtv.com/live/hebqcyy.m3u8"},
  {"重庆音乐广播","https://radio.0472.org/?id=372"},
  {"龙江音乐广播","https://radio.0472.org/?id=552"},
  {"内蒙音乐之声","https://radio.0472.org/?id=572"},
  {"宁夏音乐广播","https://radio.0472.org/?id=1701"},
  {"陕西音乐广播","https://radio.0472.org/?id=561"},
  {"青海音乐广播","https://radio.0472.org/?id=616"},
  {"山西音乐广播","https://radio.0472.org/?id=637"},
  {"山东音乐广播","https://radio.0472.org/?id=702"},
  {"安徽音乐广播","https://radio.0472.org/?id=601"},
  {"江苏经典流行","http://satellitepull.cnr.cn/live/wx32jsjdlxyy/playlist.m3u8"},
  {"浙江音乐调频","https://radio.0472.org/?id=928"},
  {"厦门音乐广播","https://radio.0472.org/?id=1897"},
  {"云南音乐广播","https://radio.0472.org/?id=592"},
  {"广西音乐台","https://radio.0472.org/?id=590"},
  {"贵州音乐广播","https://radio.0472.org/?id=435"},
  {"新疆音乐广播","https://radio.0472.org/?id=1158"},
  {"海南音乐广播","https://radio.0472.org/?id=543"},
  {"河北文艺广播","http://satellitepull.cnr.cn/live/wxhebwygb/playlist.m3u8"},
  {"RFI 法广中文","https://rfienchinois64k.ice.infomaniak.ch/rfienchinois-64.mp3"},
  {"台湾中广新闻网","https://n03.rcs.revma.com/78fm9wyy2tzuv"},
  {"BBC World Service","http://as-hls-ww-live.akamaized.net/pool_87948813/live/ww/bbc_world_service/bbc_world_service.isml/bbc_world_service-audio%3d96000.norewind.m3u8"},
  {"CNN","https://tunein.cdnstream1.com/3519_96.aac"},
  {"CNA","https://14033.live.streamtheworld.com/938NOW_PREM.aac"},
  {"GB News","https://listen-gbnews.sharp-stream.com/gbnews.mp3"},
  {"LBC News","https://icecast.thisisdax.com/LBCNewsUKMP3"},
  {"Times Radio","http://timesradio.wireless.radio/stream"},
  {"Talk Radio","https://radio.talkradio.co.uk/stream"},
  {"新加坡 CAPITAL 958","https://playerservices.streamtheworld.com/api/livestream-redirect/CAPITAL958FM_PREM.aac"},
  {"Hao FM","https://playerservices.streamtheworld.com/api/livestream-redirect/HAO_963.mp3"},
  {"Gold FM","http://22903.live.streamtheworld.com:3690/GOLD905_PREM.aac"},
  {"Money FM","https://playerservices.streamtheworld.com/api/livestream-redirect/MONEY_893AAC.aac"},
  {"Yes FM","https://22393.live.streamtheworld.com/YES933_PREM.aac"},
  {"Kiss FM","https://playerservices.streamtheworld.com/api/livestream-redirect/KISS_92AAC.aac"},
  {"NPR News","https://nprdmcoitunes.akamaized.net/hls/live/2034276/itls/playlist.m3u8"},
  {"ABC News Radio","https://mediaserviceslive.akamaized.net/hls/live/2038318/rnnsw/masterhq.m3u8"},
  {"Newstalk ZB","https://playerservices.streamtheworld.com/api/livestream-redirect/NZME_31AAC.aac"},
  {"Power FM","https://crystalout.surfernetwork.com:8001/KVSP_MP3"},
  {"Classic FM","https://ice-sov.musicradio.com/ClassicFMMP3"},
  {"BBC Radio 1","http://as-hls-ww-live.akamaized.net/pool_01505109/live/ww/bbc_radio_one/bbc_radio_one.isml/bbc_radio_one-audio%3d96000.norewind.m3u8"},
  {"BBC Radio 1 Xtra","http://as-hls-ww-live.akamaized.net/pool_92079267/live/ww/bbc_1xtra/bbc_1xtra.isml/bbc_1xtra-audio%3d96000.norewind.m3u8"},
  {"BBC Radio 1 Dance","http://as-hls-ww-live.akamaized.net/pool_62063831/live/ww/bbc_radio_one_dance/bbc_radio_one_dance.isml/bbc_radio_one_dance-audio%3d96000.norewind.m3u8"},
  {"BBC Radio 2","http://as-hls-ww-live.akamaized.net/pool_74208725/live/ww/bbc_radio_two/bbc_radio_two.isml/bbc_radio_two-audio%3d96000.norewind.m3u8"},
  {"BBC Radio 3","http://as-hls-ww-live.akamaized.net/pool_23461179/live/ww/bbc_radio_three/bbc_radio_three.isml/bbc_radio_three-audio%3d96000.norewind.m3u8"},
  {"BBC Radio 4","http://as-hls-ww-live.akamaized.net/pool_55057080/live/ww/bbc_radio_fourfm/bbc_radio_fourfm.isml/bbc_radio_fourfm-audio%3d96000.norewind.m3u8"},
  {"BBC Radio 4 Extra","http://as-hls-ww-live.akamaized.net/pool_26173715/live/ww/bbc_radio_four_extra/bbc_radio_four_extra.isml/bbc_radio_four_extra-audio%3d96000.norewind.m3u8"},
  {"BBC Radio 5 Live","http://as-hls-ww-live.akamaized.net/pool_89021708/live/ww/bbc_radio_five_live/bbc_radio_five_live.isml/bbc_radio_five_live-audio%3d96000.norewind.m3u8"},
  {"BBC Radio 6 Music","http://as-hls-ww-live.akamaized.net/pool_81827798/live/ww/bbc_6music/bbc_6music.isml/bbc_6music-audio%3d96000.norewind.m3u8"},
  {"BBC Radio Asian Network","http://as-hls-ww-live.akamaized.net/pool_22108647/live/ww/bbc_asian_network/bbc_asian_network.isml/bbc_asian_network-audio%3d96000.norewind.m3u8"},
  {"凤凰卫视中文台","https://playtv-live.ifeng.com/live/06OLEGEGM4G_audio.m3u8"},
  {"CCTV-13 新闻伴音","https://piccpndali.v.myalicdn.com/audio/cctv13_2.m3u8"},
  {"上海新闻广播","https://satellitepull.cnr.cn/live/wx32shrmgb/playlist.m3u8"},
  {"上海第一财经广播","https://lhttp.qtfm.cn/live/276/64k.mp3"},
  {"江苏新闻广播","https://satellitepull.cnr.cn/live/wx32jsxwgb/playlist.m3u8"},
  {"浙江之声","https://satellitepull.cnr.cn/live/wxzjzs/playlist.m3u8"},
  {"广东新闻广播","https://satellitepull.cnr.cn/live/wxgdxwgb/playlist.m3u8"},
  {"广东珠江经济电台","https://lhttp.qtfm.cn/live/1259/64k.mp3"},
  {"四川综合广播","https://satellitepull.cnr.cn/live/wxsczhgb/playlist.m3u8"},
  {"香港电台第一台","https://rthkradio1-live.akamaized.net/hls/live/2035313/radio1/master.m3u8"},
  {"台湾中央广播电台","https://streamak0138.akamaized.net/live0138lh-mbm9/_definst_/rti3/chunklist.m3u8"},
  {"台湾 News98","https://n17a-eu.rcs.revma.com/pntx1639ntzuv"},
  {"马来西亚 Ai FM","https://playerservices.streamtheworld.com/api/livestream-redirect/AI_FMAAC_SC"},
};
constexpr size_t kBuiltinStationCount = sizeof(kBuiltinStations) / sizeof(kBuiltinStations[0]);
// END INLINED: firmware/network_radio_v1_1_ws2812_status/builtin_stations.h


// BEGIN INLINED: firmware/network_radio_v1_1_ws2812_status/station_icons.h
struct IconStation { const char* name; const char* url; const char* logo; };
constexpr IconStation kIconStations[] = {
  {"CNR中国之声","https://ngcdn001.cnr.cn/live/zgzs/index.m3u8","6871c9e06367c84fd5a06aa0.jpg"},
  {"CNR经济之声","https://ngcdn002.cnr.cn/live/jjzs/index.m3u8","53533e5d10f22d80b41f92c0.jpg"},
  {"CNR环球资讯","https://radio.0472.org/?id=692","484c0c8c565b746ec9a6c307.jpg"},
  {"CNR音乐之声","https://radio.0472.org/?id=641","fbaf3ddf9e8577d852d644b2.jpg"},
  {"CNR交通广播","https://radio.0472.org/?id=653","33a3c40c1bc2f7661a7fc09d.jpg"},
  {"CNR文艺之声","https://radio.0472.org/?id=648","29243c56dd73a87e54b45933.jpg"},
  {"CNR湾区之声","https://radio.0472.org/?id=645","f2d3c958c8fe3f440c8edaa5.jpg"},
  {"CNR经典音乐","https://radio.0472.org/?id=642","19acd190649bfaed680445aa.jpg"},
  {"CNR台海之声","https://radio.0472.org/?id=643","ebcc901cf94e5d583da8ed61.jpg"},
  {"CNR神州之声","https://radio.0472.org/?id=644","e29397b4020c22c7bdf89821.jpg"},
  {"CNR香港之声","https://radio.0472.org/?id=646","60f03caca6cb157a05f34fe5.jpg"},
  {"CNR民族之声","https://radio.0472.org/?id=647","5ad3e33aba710b1e95d5fccb.jpg"},
  {"CNR老年之声","https://radio.0472.org/?id=649","6d90ba3e88ab2caa7b2a68b9.jpg"},
  {"CNR乡村之声","https://radio.0472.org/?id=654","788606c69c5b1cc11405cb7b.jpg"},
  {"CNR南海之声","https://radio.0472.org/?id=664","9f73af224f3a35fa64ab7b32.jpg"},
  {"北京交通广播","http://ls.qingting.fm/live/336.m3u8","1603addca02ac5664142ed64.jpg"},
  {"北京新闻广播","https://lhttp.qtfm.cn/live/339/64k.mp3","5b871d6c0e342ac97800183b.jpg"},
  {"北京文艺广播","http://ls.qingting.fm/live/333.m3u8","8b0448bb6e937dfdcd75e307.jpg"},
  {"北京城市广播","https://brtv-radiolive.rbc.cn/alive/fm1073.m3u8","3a95a52f9982e6340cb45ad2.jpg"},
  {"北京体育广播","https://brtv-radiolive.rbc.cn/alive/fm1025.m3u8","5954f4582d37d853c887a67d.jpg"},
  {"北京阳光调频","https://lhttp.qtfm.cn/live/5021739/64k.mp3","c65819c67d152630c6f1e2be.jpg"},
  {"北京经典调频","https://radio.0472.org/?id=1254","bef49f59ac1c14dcccf9e510.jpg"},
  {"CRI华语环球","http://sk.cri.cn/hyhq.m3u8","fa2b891e16339d1a08ca763f.jpg"},
  {"CRI环球资讯","https://sk.cri.cn/905.m3u8","3460bbdf6e5c11588c0acc34.jpg"},
  {"CRI南海之声","https://sk.cri.cn/nhzs.m3u8","942803712e32a906aa54b85c.jpg"},
  {"CRI英语资讯","http://sk.cri.cn/am846.m3u8","2c363ecbf3566c5847a25fb3.jpg"},
  {"RTHK3","https://rthkradio3-live.akamaized.net/hls/live/2040079/radio3/master.m3u8","dc2e2b29d8b1a6919fff1e0d.jpg"},
  {"香港电台普通话台","https://rthkradiopth-live.akamaized.net/hls/live/2040082/radiopth/master.m3u8","3d55d6e3beef84e299c59a2d.jpg"},
  {"湖南经济广播","https://radio.0472.org/?id=1056","3d0266b80490d0efb967f8e6.jpg"},
  {"湖南新闻频道","https://radio.0472.org/?id=525","2f71407ae570bd62b36d0102.jpg"},
  {"湖南潇湘之声","https://radio.0472.org/?id=526","808915ea385b3175746417a3.jpg"},
  {"重庆文艺广播","http://satellitepull.cnr.cn/live/wxcqwygb/playlist.m3u8","48780aaa2006e266b3c10cfa.jpg"},
  {"上海故事广播","http://live.cooltv.top/tv/news1296.php?id=10","17bc1b0ceb1212a3df5f2069.jpg"},
  {"山西文艺广播","http://radiolive.sxrtv.com/live/wenyi/playlist.m3u8","fac333181a112b3469434019.jpg"},
  {"江苏文艺广播","http://satellitepull.cnr.cn/live/wx32jswygb/playlist.m3u8","b9a7fda7bc703cdb3a06c2ac.jpg"},
  {"江苏故事广播","http://satellitepull.cnr.cn/live/wx32jsgsgb/playlist.m3u8","d926dca78e0bba1605ce320f.jpg"},
  {"上海戏曲广播","https://radio.0472.org/?id=1314","e02dd076a74642fc945ecbce.jpg"},
  {"陕西故事广播","https://radio.0472.org/?id=1133","fb8c996d985e787d1d2c8fac.jpg"},
  {"北京音乐广播","http://ls.qingting.fm/live/332.m3u8","bc660c9cba38392c7d5bbfc3.jpg"},
  {"湖南金鹰之声","https://radio.0472.org/?id=523","980ba8b80eba70a2b8438849.jpg"},
  {"芒果时空音乐","https://radio.0472.org/?id=524","1050aa2ab4b2adf60493889a.jpg"},
  {"湖南交通广播","https://radio.0472.org/?id=1059","0ef6098643e56f5cedcc3e69.jpg"},
  {"湖南音乐之声","https://radio.0472.org/?id=1060","39c953da121e08cbd334688a.jpg"},
  {"长沙交通广播","https://radio.0472.org/?id=1061","9cdbef7ee5b4a26ff240f668.jpg"},
  {"长沙音乐广播","https://radio.0472.org/?id=1531","bc42f66ac5130cad2d006d4a.jpg"},
  {"广东音乐之声","http://ls.qingting.fm/live/1260.m3u8","872c85048e00e9edf84679e7.jpg"},
  {"江西音乐广播","http://satellitepull.cnr.cn/live/wx32jiangxyygb/playlist.m3u8","2ee3a75bca5f658140643f87.jpg"},
  {"河北音乐广播","https://radio.0472.org/?id=381","f2414fa4ae8f72f7038c8f25.jpg"},
  {"深圳音乐频率","https://radio.0472.org/?id=498","5f81dd29eeefd78aafaa7940.jpg"},
  {"南京音乐广播","http://hls.njgb.com/live_hls/4/playlist.m3u8","9294af1b36817696e476acb2.jpg"},
  {"江苏音乐广播","https://radio.0472.org/?id=415","56739bdfc3c8817b84a804a4.jpg"},
  {"河北汽车音乐","https://radio.pull.hebtv.com/live/hebqcyy.m3u8","e28da11ec2608ad62f2fd308.jpg"},
  {"重庆音乐广播","https://radio.0472.org/?id=372","6bc81b5586d6a0572248b112.jpg"},
  {"龙江音乐广播","https://radio.0472.org/?id=552","50f09b8c2dcdfeb55bfe09e5.jpg"},
  {"内蒙音乐之声","https://radio.0472.org/?id=572","60c3e570e8b2340d0beff10f.jpg"},
  {"宁夏音乐广播","https://radio.0472.org/?id=1701","bb5c62227958fa26cc071e3e.jpg"},
  {"陕西音乐广播","https://radio.0472.org/?id=561","74b59ccf0f982f3b1710aa09.jpg"},
  {"青海音乐广播","https://radio.0472.org/?id=616","8ff1c18ce7b14b75ee39bbd4.jpg"},
  {"山西音乐广播","https://radio.0472.org/?id=637","492e8d9029cccef597d0add3.jpg"},
  {"山东音乐广播","https://radio.0472.org/?id=702","6ea194b3b461505fc6406f1c.jpg"},
  {"安徽音乐广播","https://radio.0472.org/?id=601","abf2e98edc9feb2d985476fb.jpg"},
  {"江苏经典流行","http://satellitepull.cnr.cn/live/wx32jsjdlxyy/playlist.m3u8","38fc875f711411b135042fa9.jpg"},
  {"浙江音乐调频","https://radio.0472.org/?id=928","d7737794daf2ebc945c94e31.jpg"},
  {"厦门音乐广播","https://radio.0472.org/?id=1897","cb94d50dad714abbab21922e.jpg"},
  {"云南音乐广播","https://radio.0472.org/?id=592","d7722296ba934732eec00c2b.jpg"},
  {"广西音乐台","https://radio.0472.org/?id=590","e2561d1fb54fd9b3ce2a345b.jpg"},
  {"贵州音乐广播","https://radio.0472.org/?id=435","0b0497b093c9485f47a572bf.jpg"},
  {"新疆音乐广播","https://radio.0472.org/?id=1158","4166e1b2d9af8601c24f7e9e.jpg"},
  {"海南音乐广播","https://radio.0472.org/?id=543","dc8cd36bb89b027e71bd4ff0.jpg"},
  {"河北文艺广播","http://satellitepull.cnr.cn/live/wxhebwygb/playlist.m3u8","c6fec9976f72dac4563b488b.jpg"},
  {"RFI 法广中文","https://rfienchinois64k.ice.infomaniak.ch/rfienchinois-64.mp3","58c61f222819811ba395aa68.jpg"},
  {"台湾中广新闻网","https://n03.rcs.revma.com/78fm9wyy2tzuv","b49926306dbe24feb8b39697.jpg"},
  {"BBC World Service","http://as-hls-ww-live.akamaized.net/pool_87948813/live/ww/bbc_world_service/bbc_world_service.isml/bbc_world_service-audio%3d96000.norewind.m3u8","5e495b597e158b59b5c81edc.jpg"},
  {"CNN","https://tunein.cdnstream1.com/3519_96.aac","710bf9616a6d4bb28cab5033.jpg"},
  {"CNA","https://14033.live.streamtheworld.com/938NOW_PREM.aac","bec9381eaa590281ec185823.jpg"},
  {"GB News","https://listen-gbnews.sharp-stream.com/gbnews.mp3","99f30a1b202bf28395f0b3fb.jpg"},
  {"LBC News","https://icecast.thisisdax.com/LBCNewsUKMP3","0425029d0f14e2f4d0477298.jpg"},
  {"Times Radio","http://timesradio.wireless.radio/stream","2ee00d794fd855fa69e06634.jpg"},
  {"Talk Radio","https://radio.talkradio.co.uk/stream","651588df722dc232bbfa4ffe.jpg"},
  {"新加坡 CAPITAL 958","https://playerservices.streamtheworld.com/api/livestream-redirect/CAPITAL958FM_PREM.aac","4ce3a7d28c36f3d685527c79.jpg"},
  {"Hao FM","https://playerservices.streamtheworld.com/api/livestream-redirect/HAO_963.mp3","031d9625061c02f98e11e247.jpg"},
  {"Gold FM","http://22903.live.streamtheworld.com:3690/GOLD905_PREM.aac","02ef55e0085741761d54a857.jpg"},
  {"Money FM","https://playerservices.streamtheworld.com/api/livestream-redirect/MONEY_893AAC.aac","8fd313b19ab8a01297b272a8.jpg"},
  {"Yes FM","https://22393.live.streamtheworld.com/YES933_PREM.aac","d6542e70f9a1eea065858c4b.jpg"},
  {"Kiss FM","https://playerservices.streamtheworld.com/api/livestream-redirect/KISS_92AAC.aac","4cc1dc94b70480eef309bbeb.jpg"},
  {"NPR News","https://nprdmcoitunes.akamaized.net/hls/live/2034276/itls/playlist.m3u8","e1382dec247ff614bbc7caf4.jpg"},
  {"ABC News Radio","https://mediaserviceslive.akamaized.net/hls/live/2038318/rnnsw/masterhq.m3u8","ac4826688673730cc1df7867.jpg"},
  {"Newstalk ZB","https://playerservices.streamtheworld.com/api/livestream-redirect/NZME_31AAC.aac","be98114258155f021b13ad52.jpg"},
  {"Power FM","https://crystalout.surfernetwork.com:8001/KVSP_MP3","53af1a65c6d168abcb7250c0.jpg"},
  {"Classic FM","https://ice-sov.musicradio.com/ClassicFMMP3","3cae783b85ec7ca605ef9d47.jpg"},
  {"BBC Radio 1","http://as-hls-ww-live.akamaized.net/pool_01505109/live/ww/bbc_radio_one/bbc_radio_one.isml/bbc_radio_one-audio%3d96000.norewind.m3u8","d77a061ffe1489b2b774d6d0.jpg"},
  {"BBC Radio 1 Xtra","http://as-hls-ww-live.akamaized.net/pool_92079267/live/ww/bbc_1xtra/bbc_1xtra.isml/bbc_1xtra-audio%3d96000.norewind.m3u8","906b03764a3c05c84c8b872d.jpg"},
  {"BBC Radio 1 Dance","http://as-hls-ww-live.akamaized.net/pool_62063831/live/ww/bbc_radio_one_dance/bbc_radio_one_dance.isml/bbc_radio_one_dance-audio%3d96000.norewind.m3u8","3ffdf2e0d29fe9467ba59934.jpg"},
  {"BBC Radio 2","http://as-hls-ww-live.akamaized.net/pool_74208725/live/ww/bbc_radio_two/bbc_radio_two.isml/bbc_radio_two-audio%3d96000.norewind.m3u8","7e838dd0bd66f314e88dbba0.jpg"},
  {"BBC Radio 3","http://as-hls-ww-live.akamaized.net/pool_23461179/live/ww/bbc_radio_three/bbc_radio_three.isml/bbc_radio_three-audio%3d96000.norewind.m3u8","20f4ab472d601edbb238e90a.jpg"},
  {"BBC Radio 4","http://as-hls-ww-live.akamaized.net/pool_55057080/live/ww/bbc_radio_fourfm/bbc_radio_fourfm.isml/bbc_radio_fourfm-audio%3d96000.norewind.m3u8","85d4cfe96c80d5d9bfc80c33.jpg"},
  {"BBC Radio 4 Extra","http://as-hls-ww-live.akamaized.net/pool_26173715/live/ww/bbc_radio_four_extra/bbc_radio_four_extra.isml/bbc_radio_four_extra-audio%3d96000.norewind.m3u8","aa66b900daaaae62bd86260b.jpg"},
  {"BBC Radio 5 Live","http://as-hls-ww-live.akamaized.net/pool_89021708/live/ww/bbc_radio_five_live/bbc_radio_five_live.isml/bbc_radio_five_live-audio%3d96000.norewind.m3u8","879bff327cb6aed761a5bcff.jpg"},
  {"BBC Radio 6 Music","http://as-hls-ww-live.akamaized.net/pool_81827798/live/ww/bbc_6music/bbc_6music.isml/bbc_6music-audio%3d96000.norewind.m3u8","b653593dee058ef1dd2af51b.jpg"},
  {"BBC Radio Asian Network","http://as-hls-ww-live.akamaized.net/pool_22108647/live/ww/bbc_asian_network/bbc_asian_network.isml/bbc_asian_network-audio%3d96000.norewind.m3u8","84cbaede9bd7ef503448eff3.jpg"},
  {"凤凰卫视音频","https://playtv-live.ifeng.com/live/06OLEEWQKN4_audio.m3u8","41b33863895364f62d65c44a.jpg"},
  {"凤凰卫视中文台","https://playtv-live.ifeng.com/live/06OLEGEGM4G_audio.m3u8","41b33863895364f62d65c44a.jpg"},
  {"CCTV-13 新闻伴音","https://piccpndali.v.myalicdn.com/audio/cctv13_2.m3u8","ae26be8d7a67c613ccdb4d61.jpg"},
  {"上海新闻广播","https://satellitepull.cnr.cn/live/wx32shrmgb/playlist.m3u8","1f2c6a03b8bc962ca7c11f4e.jpg"},
  {"上海第一财经广播","https://lhttp.qtfm.cn/live/276/64k.mp3","d04b0c4efde11d9e91cb84f8.jpg"},
  {"江苏新闻广播","https://satellitepull.cnr.cn/live/wx32jsxwgb/playlist.m3u8","05994bfa8b08dc3b13fd2c1e.jpg"},
  {"浙江之声","https://satellitepull.cnr.cn/live/wxzjzs/playlist.m3u8","efe50f85c34ec4f2f24a4053.jpg"},
  {"广东新闻广播","https://satellitepull.cnr.cn/live/wxgdxwgb/playlist.m3u8","b356f010e99cfd5eb9bc4c38.jpg"},
  {"广东珠江经济电台","https://lhttp.qtfm.cn/live/1259/64k.mp3","a26d44dc09283c72fe22635c.jpg"},
  {"四川综合广播","https://satellitepull.cnr.cn/live/wxsczhgb/playlist.m3u8","cc49d79e589689c9c86fd8f7.jpg"},
  {"香港电台第一台","https://rthkradio1-live.akamaized.net/hls/live/2035313/radio1/master.m3u8","8c0422de4d07d9689ce97548.jpg"},
  {"台湾中央广播电台","https://streamak0138.akamaized.net/live0138lh-mbm9/_definst_/rti3/chunklist.m3u8","c1096c41004d6a7dff7481ab.jpg"},
  {"台湾 News98","https://n17a-eu.rcs.revma.com/pntx1639ntzuv","bca58bbef5d0d78637bf8629.jpg"},
  {"马来西亚 Ai FM","https://playerservices.streamtheworld.com/api/livestream-redirect/AI_FMAAC_SC","44ecfb22282abc57067d73ed.jpg"},
};
constexpr size_t kIconStationCount=sizeof(kIconStations)/sizeof(kIconStations[0]);
// END INLINED: firmware/network_radio_v1_1_ws2812_status/station_icons.h

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
constexpr uint32_t kPlaybackStartupTimeoutMs = 30000;
constexpr uint32_t kPlaybackPostReadyRetryMs = 1000;
constexpr uint8_t kStatusLedPin = 48;
constexpr uint8_t kStatusLedBrightness = 36;
constexpr uint32_t kStatusLedRefreshMs = 20;
constexpr size_t kLegacyBuiltinStationCount = 100;
constexpr char kLegacyBuiltinCatalogKey[] = "builtin_100_v1";
constexpr char kNewsStationPackKey[] = "news_pack_v1";
constexpr char kRegionSortKey[] = "region_sort_v3";
static_assert(kBuiltinStationCount == 113,
              "Update the incremental station-pack boundary when the catalog changes.");

class St7735rDisplay {
 public:
  static constexpr uint16_t kBlack = 0x0000;
  static constexpr uint16_t kWhite = 0xFFFF;
  static constexpr uint16_t kRed = 0xF800;
  static constexpr uint16_t kGreen = 0x07E0;
  static constexpr uint16_t kBlue = 0x001F;

  void begin() {
    pinMode(config::kTftCs, OUTPUT);
    pinMode(config::kTftDc, OUTPUT);
    pinMode(config::kTftReset, OUTPUT);
    pinMode(config::kTftBacklight, OUTPUT);
    digitalWrite(config::kTftCs, HIGH);
    digitalWrite(config::kTftBacklight, LOW);
    SPI.begin(config::kTftSclk, -1, config::kTftMosi, config::kTftCs);

    digitalWrite(config::kTftReset, HIGH);
    delay(5);
    digitalWrite(config::kTftReset, LOW);
    delay(20);
    digitalWrite(config::kTftReset, HIGH);
    delay(150);

    command(0x01);  // SWRESET
    delay(150);
    command(0x11);  // SLPOUT
    delay(120);
    const uint8_t frameRate[] = {0x01, 0x2C, 0x2D};
    command(0xB1, frameRate, sizeof(frameRate));
    command(0xB2, frameRate, sizeof(frameRate));
    const uint8_t frameRateIdle[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};
    command(0xB3, frameRateIdle, sizeof(frameRateIdle));
    const uint8_t inversion[] = {0x07};
    command(0xB4, inversion, sizeof(inversion));
    const uint8_t power1[] = {0xA2, 0x02, 0x84};
    const uint8_t power2[] = {0xC5};
    const uint8_t power3[] = {0x0A, 0x00};
    const uint8_t power4[] = {0x8A, 0x2A};
    const uint8_t power5[] = {0x8A, 0xEE};
    const uint8_t vcom[] = {0x0E};
    command(0xC0, power1, sizeof(power1));
    command(0xC1, power2, sizeof(power2));
    command(0xC2, power3, sizeof(power3));
    command(0xC3, power4, sizeof(power4));
    command(0xC4, power5, sizeof(power5));
    command(0xC5, vcom, sizeof(vcom));
    const uint8_t madctl[] = {0xC8};  // 128x128 ST7735R, BGR color order.
    const uint8_t colorMode[] = {0x05};  // 16-bit RGB565.
    command(0x36, madctl, sizeof(madctl));
    command(0x3A, colorMode, sizeof(colorMode));
    command(0x20);  // INVOFF
    command(0x29);  // DISPON
    delay(100);
    digitalWrite(config::kTftBacklight, HIGH);
  }

  void runStartupSequence() {
    const uint16_t colors[] = {kRed, kGreen, kBlue, kWhite, kBlack};
    for (uint16_t color : colors) {
      fillScreen(color);
      delay(180);
    }
    drawOkPage(true);
    lastBlinkAt_ = millis();
  }

  void update() {
    if (!showingStatusPage_) return;
    const uint32_t now = millis();
    if (now - lastBlinkAt_ < 500) return;
    lastBlinkAt_ = now;
    blinkOn_ = !blinkOn_;
    fillRect(61, 121, 6, 3, blinkOn_ ? kOrange : kDimOrange);
  }

  void showStatus(const char *stationName, bool wifiConnected,
                  int32_t rssi, uint8_t volume, bool playing) {
    const uint8_t wifiPercent = wifiConnected
                                    ? static_cast<uint8_t>(constrain(map(rssi, -90, -45, 0, 100), 0, 100) / 10 * 10)
                                    : 0;
    const bool stationChanged = strncmp(stationName, stationName_, sizeof(stationName_)) != 0;
    if (showingStatusPage_ && !stationChanged && wifiPercent == wifiPercent_ &&
        volume == volume_ && playing == playing_) {
      return;
    }

    strlcpy(stationName_, stationName, sizeof(stationName_));
    wifiPercent_ = wifiPercent;
    volume_ = volume;
    playing_ = playing;
    showingStatusPage_ = true;
    blinkOn_ = true;
    lastBlinkAt_ = millis();
    drawStatusPage();
  }

 private:
  static constexpr uint8_t kWidth = 128;
  static constexpr uint8_t kHeight = 128;
  static constexpr uint8_t kColumnOffset = 2;
  static constexpr uint8_t kRowOffset = 3;
  static constexpr uint16_t kOrange = 0xFC00;
  static constexpr uint16_t kDimOrange = 0x8200;
  static constexpr uint16_t kCharcoal = 0x0841;
  static constexpr uint16_t kCard = 0xFFFF;

  uint32_t lastBlinkAt_ = 0;
  bool blinkOn_ = true;
  bool showingStatusPage_ = false;
  bool playing_ = false;
  uint8_t wifiPercent_ = 0;
  uint8_t volume_ = 0;
  char stationName_[config::kStationNameSize] = {};

  void command(uint8_t value, const uint8_t *data = nullptr, size_t length = 0) {
    SPI.beginTransaction(SPISettings(config::kTftSpiHz, MSBFIRST, SPI_MODE0));
    digitalWrite(config::kTftCs, LOW);
    digitalWrite(config::kTftDc, LOW);
    SPI.transfer(value);
    if (data != nullptr && length > 0) {
      digitalWrite(config::kTftDc, HIGH);
      SPI.writeBytes(data, length);
    }
    digitalWrite(config::kTftCs, HIGH);
    SPI.endTransaction();
  }

  void writeCommand(uint8_t value) {
    digitalWrite(config::kTftDc, LOW);
    SPI.transfer(value);
  }

  void writeData(uint8_t value) {
    digitalWrite(config::kTftDc, HIGH);
    SPI.transfer(value);
  }

  void setWindow(uint8_t x, uint8_t y, uint8_t width, uint8_t height) {
    const uint8_t x0 = x + kColumnOffset;
    const uint8_t y0 = y + kRowOffset;
    const uint8_t x1 = x0 + width - 1;
    const uint8_t y1 = y0 + height - 1;
    writeCommand(0x2A);
    writeData(0x00); writeData(x0); writeData(0x00); writeData(x1);
    writeCommand(0x2B);
    writeData(0x00); writeData(y0); writeData(0x00); writeData(y1);
    writeCommand(0x2C);
  }

  void fillScreen(uint16_t color) { fillRect(0, 0, kWidth, kHeight, color); }

  void fillRect(uint8_t x, uint8_t y, uint8_t width, uint8_t height, uint16_t color) {
    SPI.beginTransaction(SPISettings(config::kTftSpiHz, MSBFIRST, SPI_MODE0));
    digitalWrite(config::kTftCs, LOW);
    setWindow(x, y, width, height);
    digitalWrite(config::kTftDc, HIGH);
    const uint8_t high = static_cast<uint8_t>(color >> 8);
    const uint8_t low = static_cast<uint8_t>(color);
    for (uint16_t pixel = static_cast<uint16_t>(width) * height; pixel > 0; --pixel) {
      SPI.transfer(high);
      SPI.transfer(low);
    }
    digitalWrite(config::kTftCs, HIGH);
    SPI.endTransaction();
  }

  const uint8_t *glyph(char character) const {
    static constexpr uint8_t kSpace[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    static constexpr uint8_t k0[] = {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
    static constexpr uint8_t k1[] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
    static constexpr uint8_t k2[] = {0x0E, 0x11, 0x10, 0x08, 0x04, 0x02, 0x1F};
    static constexpr uint8_t k3[] = {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
    static constexpr uint8_t k4[] = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
    static constexpr uint8_t k5[] = {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
    static constexpr uint8_t k6[] = {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E};
    static constexpr uint8_t k7[] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
    static constexpr uint8_t k8[] = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
    static constexpr uint8_t k9[] = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E};
    static constexpr uint8_t kPercent[] = {0x19, 0x1A, 0x04, 0x08, 0x16, 0x13, 0x00};
    static constexpr uint8_t kA[] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    static constexpr uint8_t kC[] = {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
    static constexpr uint8_t kD[] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
    static constexpr uint8_t kE[] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    static constexpr uint8_t kT[] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    static constexpr uint8_t kF[] = {0x1F, 0x01, 0x01, 0x0F, 0x01, 0x01, 0x01};
    static constexpr uint8_t kI[] = {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E};
    static constexpr uint8_t kL[] = {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x1F};
    static constexpr uint8_t kM[] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
    static constexpr uint8_t kN[] = {0x11, 0x13, 0x15, 0x19, 0x11, 0x11, 0x11};
    static constexpr uint8_t kO[] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    static constexpr uint8_t kP[] = {0x1E, 0x11, 0x11, 0x1E, 0x01, 0x01, 0x01};
    static constexpr uint8_t kR[] = {0x1E, 0x11, 0x11, 0x1E, 0x05, 0x09, 0x11};
    static constexpr uint8_t kS[] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    static constexpr uint8_t kU[] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    static constexpr uint8_t kV[] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04};
    static constexpr uint8_t kW[] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11};
    static constexpr uint8_t kK[] = {0x11, 0x09, 0x05, 0x03, 0x05, 0x09, 0x11};
    switch (character) {
      case '0': return k0;
      case '1': return k1;
      case '2': return k2;
      case '3': return k3;
      case '4': return k4;
      case '5': return k5;
      case '6': return k6;
      case '7': return k7;
      case '8': return k8;
      case '9': return k9;
      case '%': return kPercent;
      case 'A': return kA;
      case 'C': return kC;
      case 'D': return kD;
      case 'E': return kE;
      case 'T': return kT;
      case 'F': return kF;
      case 'I': return kI;
      case 'L': return kL;
      case 'M': return kM;
      case 'N': return kN;
      case 'O': return kO;
      case 'P': return kP;
      case 'R': return kR;
      case 'S': return kS;
      case 'U': return kU;
      case 'V': return kV;
      case 'W': return kW;
      case 'K': return kK;
      default: return kSpace;
    }
  }

  void drawGlyph(uint8_t x, uint8_t y, char character, uint8_t scale, uint16_t color) {
    const uint8_t *rows = glyph(character);
    for (uint8_t row = 0; row < 7; ++row) {
      for (uint8_t column = 0; column < 5; ++column) {
        if ((rows[row] & (1U << (4U - column))) != 0) {
          fillRect(x + column * scale, y + row * scale, scale, scale, color);
        }
      }
    }
  }

  void drawText(uint8_t x, uint8_t y, const char *text, uint8_t scale, uint16_t color) {
    while (*text != '\0') {
      drawGlyph(x, y, *text++, scale, color);
      x += 6 * scale;
    }
  }

  void drawOkPage(bool indicatorOn) {
    showingStatusPage_ = false;
    blinkOn_ = indicatorOn;
    fillScreen(kBlack);
    drawText(11, 51, "TFT OK", 3, kWhite);
    fillRect(103, 94, 12, 12, blinkOn_ ? kGreen : kBlack);
  }

  void drawWifi(uint8_t x, uint8_t y, uint8_t percent) {
    const uint16_t color = percent > 0 ? kWhite : kDimOrange;
    fillRect(x + 5, y + 8, 3, 3, color);
    fillRect(x + 2, y + 5, 9, 2, color);
    fillRect(x, y + 2, 13, 2, color);
    fillRect(x + 2, y, 9, 1, color);
  }

  void drawVolume() {
    const uint8_t width = static_cast<uint8_t>(map(volume_, 0, 21, 0, 52));
    fillRect(35, 108, 58, 4, kCharcoal);
    if (width > 0) fillRect(35, 108, width, 4, kOrange);
    drawText(99, 107, String(map(volume_, 0, 21, 0, 100)).c_str(), 1, kWhite);
  }

  void drawLogoCard() {
    fillRect(20, 15, 88, 51, kCard);
    fillRect(20, 15, 88, 2, kOrange);
    const bool isCnr = strstr(stationName_, "CNR") != nullptr;
    if (isCnr) {
      drawText(27, 25, "CNR", 5, kBlack);
      fillRect(84, 27, 3, 15, kOrange);
      fillRect(89, 23, 3, 23, kOrange);
      fillRect(94, 19, 3, 31, kOrange);
      fillRect(99, 23, 3, 23, kOrange);
      fillRect(104, 27, 2, 15, kOrange);
      fillRect(27, 56, 74, 2, kOrange);
      drawText(45, 60, "MUSIC", 1, kBlack);
    } else {
      drawText(32, 28, "RADIO", 3, kBlack);
      fillRect(28, 54, 72, 2, kOrange);
      drawText(45, 59, "LIVE", 1, kBlack);
    }
  }

  void drawStatusPage() {
    fillScreen(kBlack);
    drawWifi(95, 5, wifiPercent_);
    char percent[5] = {};
    snprintf(percent, sizeof(percent), "%u%%", wifiPercent_);
    drawText(109, 7, percent, 1, kWhite);
    drawLogoCard();
    drawText(46, 71, playing_ ? "LIVE" : "WAIT", 2, playing_ ? kWhite : kDimOrange);
    fillRect(61, 88, 6, 6, kOrange);
    fillRect(63, 89, 1, 4, kBlack);
    fillRect(65, 89, 1, 4, kBlack);
    drawVolume();
    fillRect(53, 121, 3, 3, kDimOrange);
    fillRect(61, 121, 6, 3, kOrange);
    fillRect(72, 121, 3, 3, kDimOrange);
  }
};

St7735rDisplay lcd;

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
bool playbackAwaitingReady = false;
bool playbackFaultPending = false;
uint32_t playbackAttemptStartedAt = 0;
char playbackFaultMessage[96] = {};
bool wasStationConnected = false;
bool otaSucceeded = false;
bool playbackEnabled = true;
bool stationChangePending = false;
bool otaInProgress = false;
bool playbackWasEnabledBeforeOta = false;
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
         text.indexOf("connection lost") >= 0 ||
         text.indexOf("stream lost") >= 0 ||
         text.indexOf("processing stopped") >= 0 ||
         text.indexOf(" 403 ") >= 0 || text.indexOf(" 404 ") >= 0;
}

bool timeReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

void updateStatusLed(bool force = false) {
  static uint32_t lastUpdateAt = 0;
  static uint32_t lastColor = UINT32_MAX;
  const uint32_t now = millis();
  if (!force && now - lastUpdateAt < kStatusLedRefreshMs) return;
  lastUpdateAt = now;

  if (audio.isRunning() && !playbackAwaitingReady) statusLedError = false;
  StatusLedMode mode = StatusLedMode::Off;
  if (otaInProgress) mode = StatusLedMode::Ota;
  else if (statusLedError || (stationConfigured && WiFi.status() != WL_CONNECTED)) mode = StatusLedMode::Error;
  else if (audio.isRunning() && !playbackAwaitingReady) mode = StatusLedMode::Playing;
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
    String event(message.msg);
    event.toLowerCase();
    if (event.indexOf("stream ready") >= 0 && playbackEnabled) {
      // connecttohost() only means that a TCP request was issued.  Treat the
      // decoder's explicit ready event as the successful playback boundary.
      playbackAwaitingReady = false;
      playbackFaultPending = false;
      playerRequested = false;
      playbackFailures = 0;
      nextPlaybackRetryAt = millis() + kPlaybackPostReadyRetryMs;
      statusLedError = false;
    } else if (playbackEnabled && audioMessageIndicatesError(message.msg)) {
      // The callback can run inside the audio library.  Defer stopSong() and
      // reconnection to the main loop instead of tearing down the decoder from
      // inside that callback.
      strlcpy(playbackFaultMessage, message.msg, sizeof(playbackFaultMessage));
      playbackFaultPending = true;
      statusLedError = true;
    }
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
  server.requestAuthentication(BASIC_AUTH, "Network Radio 3.0 Admin");
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
  playbackAwaitingReady = false;
  playbackFaultPending = false;
  playbackFaultMessage[0] = '\0';
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
  // ESP32-audioI2S keeps its I2S channel alive after the first setPinout().
  // Re-registering that channel causes an ESP_ERR_INVALID_STATE abort, so a
  // recovery rebuild stops the decoder and reconnects only; setup owns I2S init.
  playerRequested = audio.connecttohost(stations[selectedStation].url);
  if (!playerRequested) {
    schedulePlaybackRetry("could not start audio stream");
    return false;
  }
  playbackAwaitingReady = true;
  playbackFaultPending = false;
  playbackFaultMessage[0] = '\0';
  playbackAttemptStartedAt = millis();
  statusLedError = false;
  nextPlaybackRetryAt = playbackAttemptStartedAt + kPlaybackStartupTimeoutMs;
  setPlayerMessage("connecting");
  addLog("player", reason);
  return true;
}

void onPlaylistSelectionV8(uint8_t) {
  playbackEnabled = true;
  stationChangePending = true;
  pendingPlaybackReason = "station selected";
  playerRequested = true;
  playbackAwaitingReady = false;
  playbackFaultPending = false;
  statusLedError = false;
  setPlayerMessage("switching station");
}

void maintainNetworkAndPlayback() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (!connected) {
    if (wasStationConnected) {
      audio.stopSong();
      playerRequested = false;
      playbackAwaitingReady = false;
      playbackFaultPending = false;
      setPlayerMessage("router Wi-Fi disconnected; retrying");
      addLog("wifi", "router Wi-Fi disconnected");
    }
    // WiFi.reconnect() cancels an active radio scan on ESP32.  Let the scan
    // finish before scheduling the next association attempt.
    if (WiFi.scanComplete() != WIFI_SCAN_RUNNING &&
        millis() - lastWifiRetryAt >= kWifiRetryMs) {
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
  const uint32_t now = millis();
  if (playbackFaultPending) {
    // Copy before stopSong(), whose callback can overwrite the status text.
    char reason[sizeof(playbackFaultMessage)] = {};
    strlcpy(reason, playbackFaultMessage[0] ? playbackFaultMessage :
                                       "audio stream failed", sizeof(reason));
    audio.stopSong();
    schedulePlaybackRetry(reason);
    return;
  }
  if (playbackEnabled && playbackAwaitingReady &&
      timeReached(now, playbackAttemptStartedAt + kPlaybackStartupTimeoutMs)) {
    audio.stopSong();
    schedulePlaybackRetry("audio stream start timed out");
    return;
  }
  // A stream which ended after becoming ready is rebuilt from its playlist
  // URL.  This also forces HLS to fetch the current media sequence.
  if (playbackEnabled && !playbackAwaitingReady && !audio.isRunning() &&
      timeReached(now, nextPlaybackRetryAt)) {
    startSelectedStationV8("rebuilding playback chain");
  }
}

const char *playerStateForUi() {
  if (!playbackEnabled) return "stopped";
  if (stationChangePending || playbackAwaitingReady || playerRequested ||
      playbackFailures > 0) return "buffering_or_reconnecting";
  if (audio.isRunning()) return "playing";
  return "buffering_or_reconnecting";
}

String adminPlayerStatusJson() {
  return "{\"state\":\"" + String(playerStateForUi()) +
         "\",\"volume\":" + String(playerVolume) +
         ",\"selected_station\":" + String(selectedStation) +
         ",\"playlist_revision\":" + String(playlistRevision) +
         ",\"input_buffer_bytes\":" + String(audio.inBufferFilled()) +
         ",\"sample_rate_hz\":" + String(audio.getSampleRate()) +
         ",\"bitrate\":" + String(audio.getBitRate()) +
         ",\"message\":\"" + jsonEscape(playerMessage) + "\"}";
}

void sendAdminPlayerStatus() { sendJson(adminPlayerStatusJson()); }

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
  const char *state = playerStateForUi();
  String json = "{\"firmware\":\"" + String(config::kFirmwareVersion) +
                "\",\"network\":{\"station_connected\":" +
                String(connected ? "true" : "false") + ",\"station_ip\":\"" +
                (connected ? WiFi.localIP().toString() : "") + "\",\"setup_ap_ssid\":\"" +
                String(accessPointSsid) + "\",\"setup_ap_password_is_separate\":true},\"player\":{\"state\":\"" +
                state + "\",\"selected_name\":\"" + jsonEscape(stations[selectedStation].name) +
                "\",\"volume\":" + String(playerVolume) + ",\"message\":\"" +
                jsonEscape(playerMessage) + "\",\"failures\":" + String(playbackFailures) +
                ",\"playlist_revision\":" + String(playlistRevision) +
                "},\"security\":{\"management_password_enabled\":" +
                String(adminPassword[0] ? "true" : "false") + "},\"memory\":{\"heap_free\":" +
                String(ESP.getFreeHeap()) + ",\"psram_free\":" + String(ESP.getFreePsram()) + "}}";
  sendJson(json);
}

void handlePlayerStatusV8() { if (requireAdmin()) sendAdminPlayerStatus(); }
void handlePlayerPlayV8() {
  if (!requireAdmin()) return;
  playbackEnabled = true;
  stationChangePending = true;
  pendingPlaybackReason = "play requested";
  playerRequested = true;
  playbackAwaitingReady = false;
  playbackFaultPending = false;
  statusLedError = false;
  setPlayerMessage("connecting");
  sendAdminPlayerStatus();
}
void handlePlayerStopV8() {
  if (!requireAdmin()) return;
  playbackEnabled = false;
  stationChangePending = false;
  playbackAwaitingReady = false;
  playbackFaultPending = false;
  statusLedError = false;
  audio.stopSong();
  playerRequested = false;
  setPlayerMessage("stopped by user");
  addLog("player", "stopped by user");
  sendAdminPlayerStatus();
}
void handlePlayerVolumeV8() {
  if (!requireAdmin()) return;
  const String value = server.arg("value");
  uint16_t volume = 0;
  if (value.isEmpty()) {
    sendJson("{\"error\":\"volume must be 0..21\"}", 400);
    return;
  }
  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value[index];
    if (character < '0' || character > '9') {
      sendJson("{\"error\":\"volume must be 0..21\"}", 400);
      return;
    }
    volume = volume * 10U + static_cast<uint8_t>(character - '0');
    if (volume > 21) {
      sendJson("{\"error\":\"volume must be 0..21\"}", 400);
      return;
    }
  }
  if (playerVolume != volume) {
    const uint8_t previousVolume = playerVolume;
    playerVolume = static_cast<uint8_t>(volume);
    audio.setVolume(playerVolume);
    if (!playerPreferences.begin("player", false)) {
      playerVolume = previousVolume;
      audio.setVolume(playerVolume);
      sendJson("{\"error\":\"could not save volume\"}", 500);
      return;
    }
    const bool saved = playerPreferences.putUChar("volume", playerVolume) == 1;
    playerPreferences.end();
    if (!saved) {
      playerVolume = previousVolume;
      audio.setVolume(playerVolume);
      sendJson("{\"error\":\"could not save volume\"}", 500);
      return;
    }
  }
  sendAdminPlayerStatus();
}

void handlePlayerPreviousV9() {
  if (!requireAdmin()) return;
  if (stationCount == 0) { sendJson("{\"error\":\"playlist is empty\"}", 409); return; }
  const uint8_t previousSelection = selectedStation;
  selectedStation = selectedStation == 0 ? stationCount - 1 : selectedStation - 1;
  if (!persistSelectedStation()) {
    selectedStation = previousSelection;
    sendJson("{\"error\":\"could not save selected station\"}", 500);
    return;
  }
  playbackEnabled = true;
  stationChangePending = true;
  pendingPlaybackReason = "previous station";
  playerRequested = true;
  playbackAwaitingReady = false;
  playbackFaultPending = false;
  statusLedError = false;
  setPlayerMessage("switching station");
  sendAdminPlayerStatus();
}

void handlePlayerNextV9() {
  if (!requireAdmin()) return;
  if (stationCount == 0) { sendJson("{\"error\":\"playlist is empty\"}", 409); return; }
  const uint8_t previousSelection = selectedStation;
  selectedStation = (selectedStation + 1) % stationCount;
  if (!persistSelectedStation()) {
    selectedStation = previousSelection;
    sendJson("{\"error\":\"could not save selected station\"}", 500);
    return;
  }
  playbackEnabled = true;
  stationChangePending = true;
  pendingPlaybackReason = "next station";
  playerRequested = true;
  playbackAwaitingReady = false;
  playbackFaultPending = false;
  statusLedError = false;
  setPlayerMessage("switching station");
  sendAdminPlayerStatus();
}

String userPlaylistJson() {
  String json = "{\"revision\":" + String(playlistRevision) +
                ",\"selected\":" + String(selectedStation) +
                ",\"stations\":[";
  for (uint8_t index = 0; index < stationCount; ++index) {
    if (index) json += ',';
    json += "{\"id\":" + String(index) + ",\"name\":\"" +
            jsonEscape(stations[index].name) + "\",\"logo\":\"" +
            jsonEscape(stations[index].logo) + "\"}";
  }
  return json + "]}";
}

String userPlayerStatusJson() {
  // Public player endpoints deliberately omit playerMessage and stream URLs.
  // Audio-library diagnostics can include an upstream host or redirect URL.
  return "{\"state\":\"" + String(playerStateForUi()) +
         "\",\"volume\":" + String(playerVolume) +
         ",\"selected_station\":" + String(selectedStation) +
         ",\"playlist_revision\":" + String(playlistRevision) + "}";
}

void sendUserPlayerStatus() { sendJson(userPlayerStatusJson()); }

void requestUserPlayback(const char *reason) {
  playbackEnabled = true;
  stationChangePending = true;
  pendingPlaybackReason = reason;
  playerRequested = true;
  playbackAwaitingReady = false;
  playbackFaultPending = false;
  statusLedError = false;
  setPlayerMessage("connecting");
}

bool selectStationForUser(uint8_t id, const char *reason) {
  const uint8_t previousSelection = selectedStation;
  selectedStation = id;
  if (!persistSelectedStation()) {
    selectedStation = previousSelection;
    return false;
  }
  requestUserPlayback(reason);
  return true;
}

void handleUserSelectStation() {
  uint8_t id;
  if (!parseStationId(id)) {
    sendJson("{\"error\":\"invalid station id\"}", 400);
    return;
  }
  if (!selectStationForUser(id, "station selected from user page")) {
    sendJson("{\"error\":\"could not save selected station\"}", 500);
    return;
  }
  sendUserPlayerStatus();
}

void handleUserPlayerStatus() { sendUserPlayerStatus(); }

void handleUserPlay() {
  requestUserPlayback("play requested from user page");
  sendUserPlayerStatus();
}

void handleUserStop() {
  playbackEnabled = false;
  stationChangePending = false;
  playbackAwaitingReady = false;
  playbackFaultPending = false;
  statusLedError = false;
  audio.stopSong();
  playerRequested = false;
  setPlayerMessage("stopped by user");
  addLog("player", "stopped from user page");
  sendUserPlayerStatus();
}

void handleUserPrevious() {
  if (stationCount == 0) { sendJson("{\"error\":\"playlist is empty\"}", 409); return; }
  const uint8_t target = selectedStation == 0 ? stationCount - 1 : selectedStation - 1;
  if (!selectStationForUser(target, "previous station from user page")) {
    sendJson("{\"error\":\"could not save selected station\"}", 500);
    return;
  }
  sendUserPlayerStatus();
}

void handleUserNext() {
  if (stationCount == 0) { sendJson("{\"error\":\"playlist is empty\"}", 409); return; }
  const uint8_t target = (selectedStation + 1) % stationCount;
  if (!selectStationForUser(target, "next station from user page")) {
    sendJson("{\"error\":\"could not save selected station\"}", 500);
    return;
  }
  sendUserPlayerStatus();
}

void handleUserVolume() {
  const String value = server.arg("value");
  if (value.isEmpty()) {
    sendJson("{\"error\":\"volume must be 0..21\"}", 400);
    return;
  }
  uint16_t volume = 0;
  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value[index];
    if (character < '0' || character > '9') {
      sendJson("{\"error\":\"volume must be 0..21\"}", 400);
      return;
    }
    volume = volume * 10U + static_cast<uint8_t>(character - '0');
    if (volume > 21) {
      sendJson("{\"error\":\"volume must be 0..21\"}", 400);
      return;
    }
  }
  if (playerVolume != volume) {
    const uint8_t previousVolume = playerVolume;
    playerVolume = static_cast<uint8_t>(volume);
    audio.setVolume(playerVolume);
    if (!playerPreferences.begin("player", false)) {
      playerVolume = previousVolume;
      audio.setVolume(playerVolume);
      sendJson("{\"error\":\"could not save volume\"}", 500);
      return;
    }
    const bool saved = playerPreferences.putUChar("volume", playerVolume) == 1;
    playerPreferences.end();
    if (!saved) {
      playerVolume = previousVolume;
      audio.setVolume(playerVolume);
      sendJson("{\"error\":\"could not save volume\"}", 500);
      return;
    }
  }
  sendUserPlayerStatus();
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

  if (target == id) {
    sendJson(playlistJson());
    return;
  }

  const uint8_t previousSelection = selectedStation;
  Station moved = {};
  if (target != id) {
    moved = stations[id];
    const bool movedStationWasSelected = selectedStation == id;
    if (target < id) {
      for (int index = id; index > target; --index) stations[index] = stations[index - 1];
      if (!movedStationWasSelected && selectedStation >= target && selectedStation < id) {
        ++selectedStation;
      }
    } else {
      for (uint8_t index = id; index < target; ++index) stations[index] = stations[index + 1];
      if (!movedStationWasSelected && selectedStation > id && selectedStation <= target) {
        --selectedStation;
      }
    }
    stations[target] = moved;
    if (movedStationWasSelected) selectedStation = target;
  }

  const bool saved = persistPlaylist();
  if (!saved && target != id) {
    if (target < id) {
      for (uint8_t index = target; index < id; ++index) {
        stations[index] = stations[index + 1];
      }
    } else {
      for (uint8_t index = target; index > id; --index) {
        stations[index] = stations[index - 1];
      }
    }
    stations[id] = moved;
    selectedStation = previousSelection;
  }
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
  if (!playerPreferences.begin(kSecurityNamespace, false)) {
    sendJson("{\"error\":\"could not open security settings\"}", 500);
    return;
  }
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
  if (playlistStorageReady) {
    if (LittleFS.exists(kPlaylistSlotA)) ok = LittleFS.remove(kPlaylistSlotA) && ok;
    if (LittleFS.exists(kPlaylistSlotB)) ok = LittleFS.remove(kPlaylistSlotB) && ok;
  }
  if (!ok) { sendJson("{\"error\":\"could not clear all settings\"}", 500); return; }
  sendJson("{\"reset\":true,\"restarting\":true}");
  delay(300); ESP.restart();
}

void resumePlaybackAfterFailedOta() {
  if (!playbackWasEnabledBeforeOta) return;
  playbackWasEnabledBeforeOta = false;
  playbackEnabled = true;
  stationChangePending = true;
  pendingPlaybackReason = "resuming after failed firmware upload";
  playerRequested = true;
  playbackAwaitingReady = false;
  playbackFaultPending = false;
  statusLedError = false;
  addLog("ota", "resuming playback after failed upload");
}

void failOtaUpload(const char *reason) {
  if (Update.isRunning()) Update.abort();
  otaSucceeded = false;
  otaInProgress = false;
  statusLedError = true;
  otaError = reason;
  addLog("ota", reason);
  resumePlaybackAfterFailedOta();
}

void handleOtaUpload() {
  if (!isAdminRequest()) return;
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    otaSucceeded = false;
    otaInProgress = true;
    playbackWasEnabledBeforeOta = playbackEnabled;
    statusLedError = false;
    otaError = "";
    // Pausing the decoder here avoids audio/OTA contention; it does not
    // change the legacy OTA request or image-size contract.
    playbackEnabled = false;
    stationChangePending = false;
    playbackAwaitingReady = false;
    playbackFaultPending = false;
    audio.stopSong();
    playerRequested = false;
    addLog("ota", "firmware upload started");
    updateStatusLed(true);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      const String error = Update.errorString();
      failOtaUpload(error.c_str());
    }
  } else if (upload.status == UPLOAD_FILE_WRITE && otaInProgress && otaError.isEmpty()) {
    updateStatusLed();
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      const String error = Update.errorString();
      failOtaUpload(error.c_str());
    }
  } else if (upload.status == UPLOAD_FILE_END && otaInProgress && otaError.isEmpty()) {
    otaSucceeded = Update.end(true);
    otaInProgress = false;
    if (!otaSucceeded) {
      otaError = Update.errorString();
      statusLedError = true;
      resumePlaybackAfterFailedOta();
    } else {
      playbackWasEnabledBeforeOta = false;
    }
    addLog("ota", otaSucceeded ? "firmware verified" : otaError.c_str());
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    failOtaUpload("firmware upload aborted");
  }
  if (upload.status != UPLOAD_FILE_WRITE) updateStatusLed(true);
}

void handleOtaResult() {
  if (!requireAdmin()) return;
  if (!otaSucceeded) {
    sendJson("{\"error\":\"" + jsonEscape(otaError.isEmpty() ? "OTA failed" : otaError) + "\"}", 500);
    return;
  }
  sendJson("{\"updated\":true,\"restarting\":true}");
  delay(500);
  ESP.restart();
}

constexpr char kUserHtmlV301[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"><meta name="theme-color" content="#656b6a"><title>网络收音机</title><style>
:root{color-scheme:dark;--bg:#656b6a;--panel:#707675;--text:#fff;--muted:#d7dcda;--line:#858b89;--accent:#f2a51a}*{box-sizing:border-box}body{margin:0;background:#4e5453;color:var(--text);font-family:-apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif}.app{position:relative;width:100%;max-width:720px;min-height:100vh;margin:auto;padding:20px clamp(18px,5vw,42px) 50px;background:var(--bg);box-shadow:0 0 32px #0004}.settings{position:absolute;right:18px;top:16px;display:grid;place-items:center;width:48px;height:48px;border:0;border-radius:50%;background:#ffffff1c;color:#fff;text-decoration:none;font-size:27px}.settings:active{transform:scale(.96)}.hero{text-align:center;padding-top:58px}.cover-wrap{position:relative;width:min(48vw,250px);aspect-ratio:1;margin:auto;border-radius:22px;background:#f5f5f5;overflow:hidden;box-shadow:0 8px 25px #0003}.cover{display:block;width:100%;height:100%;object-fit:contain;object-position:center}.cover-fallback{position:absolute;inset:0;display:none;place-items:center;background:linear-gradient(145deg,#f5a623,#d47b13);font-size:clamp(46px,12vw,78px);font-weight:800}.station-name{min-height:1.5em;margin:25px 0 5px;font-size:clamp(25px,5vw,34px);font-weight:700}.state{color:var(--accent);font-size:18px}.progress{height:7px;margin:34px 0 28px;background:#a7adaa;border-radius:10px;overflow:hidden}.progress i{display:block;width:0;height:100%;background:var(--accent);transition:width .4s}.progress.busy i{width:58%;animation:load 1.5s ease-in-out infinite}@keyframes load{0%{transform:translateX(-110%)}100%{transform:translateX(180%)}}.controls{display:flex;align-items:center;justify-content:space-around;max-width:530px;margin:auto}.controls button{display:grid;place-items:center;border:0;color:#fff;background:transparent;cursor:pointer}.controls button:not(.play){width:80px;height:70px;font-size:42px}.controls .play{width:108px;height:108px;border-radius:50%;background:#fff;color:#5e6463;font-size:48px;box-shadow:0 7px 22px #0003}.volume{display:flex;align-items:center;gap:13px;margin:30px 4px 24px;color:var(--muted)}input[type=range]{width:100%;accent-color:var(--accent)}.list-title{display:flex;align-items:center;justify-content:space-between;margin:15px 0 5px}.list-title h2{font-size:18px;margin:0}.count{color:var(--muted);font-size:14px}.station-list{border-top:1px solid var(--line)}.station{display:flex;align-items:center;gap:17px;width:100%;min-height:88px;padding:12px 10px;border:0;border-bottom:1px solid var(--line);background:transparent;color:#fff;text-align:left;cursor:pointer}.station.active{background:#ffffff12;border-left:4px solid var(--accent);padding-left:6px}.station img,.station .fallback{display:block;flex:0 0 62px;width:62px;height:62px;border-radius:13px;background:#f7f7f7;object-fit:contain;object-position:center}.station .fallback{display:grid;place-items:center;background:linear-gradient(145deg,#f5a623,#d47b13);color:#fff;font-size:25px;font-weight:800}.station b{font-size:19px;font-weight:600}.station small{display:block;margin-top:4px;color:var(--muted)}.notice{padding:30px 8px;text-align:center;color:var(--muted)}@media(max-width:480px){.app{padding-left:16px;padding-right:16px}.hero{padding-top:50px}.cover-wrap{width:56vw}.station-name{font-size:25px}.controls .play{width:94px;height:94px}.controls button:not(.play){font-size:34px}.station{min-height:78px}.station img,.station .fallback{flex-basis:54px;width:54px;height:54px}}</style></head>
<body><main class="app"><a class="settings" href="/admin" aria-label="进入管理页面" title="设置">⚙</a><section class="hero"><div class="cover-wrap"><img id="cover" class="cover" alt="当前电台台标"><div id="coverFallback" class="cover-fallback">R</div></div><div id="stationName" class="station-name">加载中…</div><div id="state" class="state">正在连接设备</div></section><div id="progress" class="progress"><i></i></div><nav class="controls" aria-label="播放控制"><button id="previous" aria-label="上一台">◀</button><button id="play" class="play" aria-label="播放或暂停">▶</button><button id="next" aria-label="下一台">▶</button></nav><div class="volume"><span>🔉</span><input id="volume" type="range" min="0" max="21" aria-label="音量"><span>🔊</span></div><div class="list-title"><h2>电台列表</h2><span id="count" class="count"></span></div><section id="stations" class="station-list"></section></main>
<script>
const q=s=>document.querySelector(s),textureStyles={none:['none','auto'],dots:['radial-gradient(#ffffff24 1px,transparent 1px)','18px 18px'],grid:['linear-gradient(#ffffff16 1px,transparent 1px),linear-gradient(90deg,#ffffff16 1px,transparent 1px)','24px 24px'],diagonal:['repeating-linear-gradient(135deg,#ffffff0d 0 2px,transparent 2px 12px)','auto'],cloud:['radial-gradient(circle at 12px 14px,transparent 9px,#ffffff1f 10px 11px,transparent 12px),radial-gradient(circle at 28px 14px,transparent 9px,#ffffff1f 10px 11px,transparent 12px)','40px 28px'],lattice:['linear-gradient(45deg,#ffffff14 12.5%,transparent 12.5% 37.5%,#ffffff14 37.5% 62.5%,transparent 62.5% 87.5%,#ffffff14 87.5%)','32px 32px'],waves:['radial-gradient(ellipse at 50% 100%,transparent 11px,#ffffff1c 12px 13px,transparent 14px)','34px 18px'],bamboo:['repeating-linear-gradient(90deg,transparent 0 30px,#ffffff16 31px 33px,transparent 34px 62px),repeating-linear-gradient(0deg,transparent 0 54px,#ffffff0d 55px 57px,transparent 58px 86px)','64px 88px'],ricepaper:['linear-gradient(25deg,#ffffff0a 1px,transparent 1px),linear-gradient(115deg,#ffffff08 1px,transparent 1px)','37px 53px,41px 47px'],porcelain:['radial-gradient(circle at 0 0,transparent 15px,#ffffff20 16px 17px,transparent 18px),radial-gradient(circle at 100% 100%,transparent 15px,#ffffff20 16px 17px,transparent 18px)','40px 40px']};
let stations=[],selected=-1,playerState='stopped',playlistRevision=0,refreshBusy=false,volumeTimer;
async function api(url,options){const r=await fetch(url,options);const t=await r.text();let d={};try{d=t?JSON.parse(t):{}}catch(_){d={error:t||'请求失败'}}if(!r.ok)throw Error(d.error||'请求失败');return d}
function safeLogo(name){return typeof name==='string'&&/^[A-Za-z0-9._-]+$/.test(name)?'/logos/'+encodeURIComponent(name):''}
function setImage(image,fallback,station){const name=(station&&station.name||'R').trim().slice(0,1).toUpperCase();fallback.textContent=name||'R';const url=safeLogo(station&&station.logo);if(!url){image.style.display='none';fallback.style.display='grid';return}image.style.display='block';fallback.style.display='none';image.onerror=()=>{image.style.display='none';fallback.style.display='grid'};image.src=url}
function updateRows(){document.querySelectorAll('[data-station-id]').forEach(row=>row.classList.toggle('active',Number(row.dataset.stationId)===selected))}
function renderNow(){const station=stations.find(s=>s.id===selected)||{name:'网络收音机',logo:''};q('#stationName').textContent=station.name;q('#play').textContent=playerState==='playing'?'Ⅱ':'▶';q('#state').textContent=({playing:'正在播放',buffering_or_reconnecting:'正在缓冲',stopped:'已暂停'})[playerState]||'正在恢复连接';q('#progress').classList.toggle('busy',playerState!=='playing'&&playerState!=='stopped');setImage(q('#cover'),q('#coverFallback'),station);updateRows()}
function renderStations(){const host=q('#stations');host.replaceChildren();q('#count').textContent=stations.length+' 个电台';if(!stations.length){const e=document.createElement('div');e.className='notice';e.textContent='暂无电台，请到管理页面添加';host.append(e);return}stations.forEach(station=>{const row=document.createElement('button'),img=document.createElement('img'),fallback=document.createElement('span'),text=document.createElement('span'),name=document.createElement('b');row.className='station';row.dataset.stationId=station.id;row.addEventListener('click',()=>selectStation(station.id));fallback.className='fallback';name.textContent=station.name;text.append(name);if(station.id===selected){const hint=document.createElement('small');hint.textContent='当前电台';text.append(hint)}setImage(img,fallback,station);row.append(img,fallback,text);host.append(row)});updateRows()}
function applyPlayer(data){playerState=data.state||playerState;if(Number.isInteger(data.selected_station))selected=data.selected_station;if(Number.isInteger(data.volume))q('#volume').value=data.volume;renderNow()}
async function loadStations(){const data=await api('/api/user/stations');stations=Array.isArray(data.stations)?data.stations:[];selected=data.selected;playlistRevision=data.revision||0;renderStations();renderNow()}
async function command(url){try{applyPlayer(await api(url,{method:'POST'}))}catch(e){alert(e.message)}}
function selectStation(id){selected=id;playerState='buffering_or_reconnecting';renderStations();renderNow();command('/api/user/stations/select?id='+encodeURIComponent(id))}
async function refresh(){if(refreshBusy)return;refreshBusy=true;try{const data=await api('/api/user/player/status');applyPlayer(data);if(data.playlist_revision!==playlistRevision)await loadStations()}catch(e){q('#state').textContent='设备连接失败'}finally{refreshBusy=false;setTimeout(refresh,document.hidden?30000:5000)}}
q('#play').addEventListener('click',()=>command(playerState==='playing'?'/api/user/player/stop':'/api/user/player/play'));q('#previous').addEventListener('click',()=>command('/api/user/player/previous'));q('#next').addEventListener('click',()=>command('/api/user/player/next'));q('#volume').addEventListener('input',e=>{clearTimeout(volumeTimer);volumeTimer=setTimeout(()=>command('/api/user/player/volume?value='+encodeURIComponent(e.target.value)),180)});
document.addEventListener('visibilitychange',()=>{if(!document.hidden)refresh()});api('/api/user/theme').then(theme=>{document.documentElement.style.setProperty('--bg',theme.background);document.documentElement.style.setProperty('--accent',theme.accent);document.body.style.backgroundColor=theme.background;q('meta[name="theme-color"]').content=theme.background;const t=textureStyles[theme.texture]||textureStyles.none;q('.app').style.backgroundImage=t[0];q('.app').style.backgroundSize=t[1]}).catch(()=>{});loadStations().then(refresh).catch(e=>{const n=document.createElement('div');n.className='notice';n.textContent=e.message;q('#stations').replaceChildren(n)});
</script></body></html>
)HTML";

constexpr char kAdminHtmlV302[] PROGMEM =
R"HTML(<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>网络收音机 3.1.0 管理</title><style>
:root{color-scheme:dark}body{max-width:880px;margin:24px auto;padding:0 16px;background:#101827;color:#e5e7eb;font:16px system-ui,-apple-system,"PingFang SC","Microsoft YaHei",sans-serif}section,pre,.station{background:#172234;padding:14px;border-radius:10px;margin:14px 0}button,input,select{box-sizing:border-box;padding:9px;margin:4px;border:0;border-radius:6px}input,select{width:100%}button{background:#38bdf8;color:#062032;font-weight:700;cursor:pointer}.warn{background:#fbbf24}.danger{background:#fb7185}.station img,.station .fallback{display:inline-grid;width:48px;height:48px;object-fit:contain;object-position:center;background:#fff;border-radius:8px;vertical-align:middle;margin-right:10px}.station .fallback{place-items:center;background:#e89c27;color:#fff;font-weight:700}.station small{display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:#b7c6da}.actions{display:block}.station button{min-width:82px;padding:11px 17px}.active{outline:2px solid #38bdf8}.state{font-size:1.1em;color:#67e8f9;margin-bottom:24px}.transport{display:flex;align-items:center;justify-content:center;gap:clamp(28px,8vw,72px);margin:18px 0 28px}.transport button{display:grid;place-items:center;margin:0}.skip{width:76px;height:64px;border-radius:18px;font-size:25px;background:#263449;color:#dce6f5}.play{width:92px;height:92px;border-radius:50%;font-size:36px;background:#f8fafc;color:#172234;box-shadow:0 10px 28px #0005}.volume-head{display:flex;justify-content:space-between;align-items:center;margin:0 6px 8px;color:#cbd5e1}.volume-head b{color:#fff;font-size:1.15em}.volume{width:calc(100% - 10px);accent-color:#38bdf8}.theme-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}.theme-grid label{display:grid;gap:6px}.theme-grid input,.theme-grid select{margin:0}.theme-grid input[type=color]{height:54px;padding:4px;border:1px solid #ffffff26;border-radius:10px;background:#fff;color-scheme:light;cursor:pointer}.theme-grid input[type=color]::-webkit-color-swatch-wrapper{padding:0}.theme-grid input[type=color]::-webkit-color-swatch{border:0;border-radius:6px}.theme-grid input[type=color]::-moz-color-swatch{border:0;border-radius:6px}pre{overflow:auto;white-space:pre-wrap}a{color:#67e8f9}@media(max-width:560px){.theme-grid{grid-template-columns:1fr}.station{overflow-x:auto;white-space:nowrap}.station small{white-space:normal}.station button{min-width:auto;padding:9px 11px;margin:3px 2px}}</style></head>
<body><h1>ESP32-S3 网络收音机</h1><p>版本号：)HTML"
NETWORK_RADIO_VERSION
R"HTML(　编译时间：)HTML"
__DATE__ " " __TIME__
R"HTML(　<a href="/">返回播放器</a></p>
<section class="player"><h2>正在播放</h2><div id="now" class="state">读取中…</div><div class="transport"><button id="previous" class="skip" aria-label="上一台">◀◀</button><button id="play" class="play" aria-label="播放或暂停">▶</button><button id="next" class="skip" aria-label="下一台">▶▶</button></div><div class="volume-head"><span>音量</span><b><span id="volumeText">--</span>/21</b></div><input id="volume" class="volume" type="range" min="0" max="21"></section>
<section><h2>用户页面外观</h2><div class="theme-grid"><label>页面颜色<input id="background" type="color" value="#656b6a"></label><label>强调颜色<input id="accent" type="color" value="#f2a51a"></label><label>纹理效果<select id="texture"><option value="none">无纹理</option><option value="dots">圆点</option><option value="grid">网格</option><option value="diagonal">斜纹</option><option value="cloud">祥云</option><option value="lattice">回纹窗格</option><option value="waves">水波</option><option value="bamboo">竹影</option><option value="ricepaper">宣纸</option><option value="porcelain">青花</option></select></label></div><button id="saveTheme">保存页面外观</button></section>
<section><h2>播放列表</h2><div id="stations">加载中…</div><h3 id="formTitle">新增电台</h3><input id="editId" type="hidden"><input id="stationName" placeholder="电台名称"><input id="stationUrl" placeholder="http(s):// 音频流地址"><button id="saveStation">保存</button><button id="cancelEdit" class="warn">取消编辑</button></section>
<section><h2>Wi-Fi</h2><button id="scanWifi">扫描网络</button><select id="ssid"><option value="">选择 Wi-Fi</option></select><input id="wifiPassword" type="password" placeholder="Wi-Fi 密码"><button id="saveWifi">保存并连接</button><button id="forgetWifi" class="warn">清除 Wi-Fi 设置</button></section>
<section><h2>维护与安全</h2><button id="chooseFirmware">选择固件并升级</button><input id="firmware" type="file" accept=".bin" hidden><button id="downloadLog">下载诊断日志</button><input id="adminPassword" type="password" placeholder="设置管理密码（8–63 位，用户名 admin）"><button id="savePassword">保存管理密码</button><button id="factoryReset" class="danger">恢复出厂设置</button><p>配网热点密码独立：<code>radio-setup</code></p></section><pre id="status">读取中…</pre>
<script>
const q=s=>document.querySelector(s),enc=o=>new URLSearchParams(o);let stations=[],selected=-1,playerState='stopped',playlistRevision=0,pollBusy=false,volumeTimer,pollCount=0;
async function api(url,options){const r=await fetch(url,options);const t=await r.text();let d={};try{d=t?JSON.parse(t):{}}catch(_){d={error:t||'请求失败'}}if(!r.ok)throw Error(d.error||'请求失败');return d}
function safeLogo(name){return typeof name==='string'&&/^[A-Za-z0-9._-]+$/.test(name)?'/logos/'+encodeURIComponent(name):''}
function button(label,handler,style){const b=document.createElement('button');b.textContent=label;if(style)b.className=style;b.addEventListener('click',handler);return b}
function stationImage(station){const image=document.createElement('img'),fallback=document.createElement('span'),url=safeLogo(station.logo);fallback.className='fallback';fallback.textContent=(station.name||'R').trim().slice(0,1).toUpperCase()||'R';if(!url){image.style.display='none'}else{fallback.style.display='none';image.onerror=()=>{image.style.display='none';fallback.style.display='grid'};image.src=url}return [image,fallback]}
function renderStations(){const host=q('#stations');host.replaceChildren();stations.forEach(station=>{const row=document.createElement('article'),title=document.createElement('b'),url=document.createElement('small'),actions=document.createElement('div'),[image,fallback]=stationImage(station);row.className='station'+(station.id===selected?' active':'');row.dataset.stationId=String(station.id);title.textContent=station.name;url.textContent=station.url;actions.className='actions';actions.append(button('播放此台',()=>post('/api/stations/select?id='+station.id)),button('编辑',()=>editStation(station.id)),button('最前',()=>post('/api/stations/move?id='+station.id+'&direction=first')),button('↑',()=>post('/api/stations/move?id='+station.id+'&direction=up')),button('↓',()=>post('/api/stations/move?id='+station.id+'&direction=down')),button('最后',()=>post('/api/stations/move?id='+station.id+'&direction=last')),button('删除',()=>removeStation(station.id),'warn'));row.append(image,fallback,title,url,actions);host.append(row)});if(!stations.length){const p=document.createElement('p');p.textContent='暂无电台';host.append(p)}}
function applyPlaylist(data){if(!Array.isArray(data.stations))return;stations=data.stations;selected=data.selected;playlistRevision=data.revision||playlistRevision;renderStations()}
function applyPlayer(data){playerState=data.state||playerState;if(Number.isInteger(data.selected_station))selected=data.selected_station;if(Number.isInteger(data.volume)){q('#volume').value=data.volume;q('#volumeText').textContent=data.volume}const current=stations.find(s=>s.id===selected);q('#now').textContent=(data.state||'stopped')+' · '+(current?current.name:'网络收音机')+(data.message?' · '+data.message:'');q('#play').textContent=playerState==='playing'?'Ⅱ':'▶';document.querySelectorAll('.station').forEach(row=>row.classList.toggle('active',Number(row.dataset.stationId)===selected))}
async function loadPlaylist(){applyPlaylist(await api('/api/stations'))}
async function refreshPlayer(){const data=await api('/api/player/status');applyPlayer(data);if(data.playlist_revision!==playlistRevision)await loadPlaylist()}
async function refreshStatus(){const data=await api('/api/status');q('#status').textContent=JSON.stringify(data,null,2)}
async function poll(){if(pollBusy)return;pollBusy=true;try{await refreshPlayer();if(++pollCount%6===0)await refreshStatus()}catch(e){q('#status').textContent='错误：'+e.message}finally{pollBusy=false;setTimeout(poll,document.hidden?30000:5000)}}
async function post(url){try{const data=await api(url,{method:'POST'});applyPlaylist(data);applyPlayer(data)}catch(e){alert(e.message)}}
function editStation(id){const s=stations.find(x=>x.id===id);if(!s)return;q('#editId').value=id;q('#stationName').value=s.name;q('#stationUrl').value=s.url;q('#formTitle').textContent='编辑电台'}
function clearForm(){q('#editId').value='';q('#stationName').value='';q('#stationUrl').value='';q('#formTitle').textContent='新增电台'}
async function saveStation(){const id=q('#editId').value,body=enc({name:q('#stationName').value,url:q('#stationUrl').value});try{const data=await api(id===''?'/api/stations':'/api/stations/update?id='+encodeURIComponent(id),{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});applyPlaylist(data);clearForm()}catch(e){alert(e.message)}}
function removeStation(id){if(confirm('删除该电台？'))post('/api/stations/delete?id='+id)}
async function scanWifi(){try{let data;for(let attempt=0;attempt<25;attempt++){data=await api('/api/wifi/scan');if(!data.scanning)break;await new Promise(resolve=>setTimeout(resolve,350))}if(!data||data.scanning)throw Error('Wi-Fi 扫描超时');const select=q('#ssid');select.replaceChildren();const empty=document.createElement('option');empty.value='';empty.textContent='选择 Wi-Fi';select.append(empty);(data.networks||[]).forEach(network=>{const option=document.createElement('option');option.value=network.ssid;option.textContent=network.ssid+' ('+network.rssi+' dBm)';select.append(option)})}catch(e){alert(e.message)}}
async function saveWifi(){try{await api('/api/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc({ssid:q('#ssid').value,password:q('#wifiPassword').value})});q('#status').textContent='Wi-Fi 已保存，设备正在重启…'}catch(e){alert(e.message)}}
async function saveTheme(){try{const data=await api('/api/ui-theme',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc({background:q('#background').value,accent:q('#accent').value,texture:q('#texture').value})});q('#background').value=data.background;q('#accent').value=data.accent;q('#texture').value=data.texture;alert('页面外观已保存')}catch(e){alert(e.message)}}
async function savePassword(){try{await api('/api/security/password',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc({password:q('#adminPassword').value})});alert('管理密码已保存；请刷新页面并用 admin 登录。')}catch(e){alert(e.message)}}
async function uploadFirmware(file){if(!file||!confirm('上传后设备会重启，继续？'))return;const form=new FormData;form.append('firmware',file);try{await api('/api/ota',{method:'POST',body:form});q('#status').textContent='升级完成，设备正在重启…'}catch(e){alert(e.message)}}
q('#previous').addEventListener('click',()=>post('/api/player/previous'));q('#play').addEventListener('click',()=>post(playerState==='playing'?'/api/player/stop':'/api/player/play'));q('#next').addEventListener('click',()=>post('/api/player/next'));q('#volume').addEventListener('input',e=>{q('#volumeText').textContent=e.target.value;clearTimeout(volumeTimer);volumeTimer=setTimeout(()=>post('/api/player/volume?value='+encodeURIComponent(e.target.value)),180)});q('#saveStation').addEventListener('click',saveStation);q('#cancelEdit').addEventListener('click',clearForm);q('#scanWifi').addEventListener('click',scanWifi);q('#saveWifi').addEventListener('click',saveWifi);q('#forgetWifi').addEventListener('click',()=>{if(confirm('清除保存的 Wi-Fi？'))post('/api/wifi/forget')});q('#saveTheme').addEventListener('click',saveTheme);q('#savePassword').addEventListener('click',savePassword);q('#chooseFirmware').addEventListener('click',()=>q('#firmware').click());q('#firmware').addEventListener('change',e=>uploadFirmware(e.target.files[0]));q('#downloadLog').addEventListener('click',()=>location='/api/diagnostics/download');q('#factoryReset').addEventListener('click',()=>{if(confirm('这将清除 Wi-Fi、电台、音量、页面外观和管理密码，确定？'))post('/api/factory-reset')});document.addEventListener('visibilitychange',()=>{if(!document.hidden)poll()});Promise.all([loadPlaylist(),refreshPlayer(),refreshStatus(),api('/api/ui-theme').then(t=>{q('#background').value=t.background;q('#accent').value=t.accent;q('#texture').value=t.texture})]).then(poll).catch(e=>q('#status').textContent='错误：'+e.message);
</script></body></html>
)HTML";

void configureWebServerV8() {
  server.serveStatic("/logos/", LittleFS, "/logos/");
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html; charset=utf-8", kUserHtmlV301); });
  server.on("/admin", HTTP_GET, [] { if (requireAdmin()) server.send_P(200, "text/html; charset=utf-8", kAdminHtmlV302); });
  server.on("/api/user/stations", HTTP_GET, [] { sendJson(userPlaylistJson()); });
  server.on("/api/user/theme", HTTP_GET, [] { sendJson(uiThemeJson()); });
  server.on("/api/user/stations/select", HTTP_POST, handleUserSelectStation);
  server.on("/api/user/player/status", HTTP_GET, handleUserPlayerStatus);
  server.on("/api/user/player/play", HTTP_POST, handleUserPlay);
  server.on("/api/user/player/stop", HTTP_POST, handleUserStop);
  server.on("/api/user/player/volume", HTTP_POST, handleUserVolume);
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
  server.on("/api/wifi", HTTP_POST, handleSaveWifi);
  server.on("/api/wifi/forget", HTTP_POST, handleForgetWifi);
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
  lcd.begin();
  lcd.runStartupSequence();
  Serial.println("ST7735R LCD startup test complete.");
  snprintf(accessPointSsid, sizeof(accessPointSsid), "%s%04X", config::kAccessPointPrefix, setupAccessPointId());
  loadSecurity();
  if (!allocateStationStore()) {
    rgbLedWrite(kStatusLedPin, kStatusLedBrightness, 0, 0);
    Serial.println("FATAL: Network Radio requires PSRAM for its station store.");
    while (true) delay(1000);
  }
  playlistStorageReady = LittleFS.begin(false);
  Serial.printf("LittleFS: %s\n", playlistStorageReady ? "mounted" : "mount failed; using NVS fallback");
  loadPlaylist();
  migrateStationCatalog();
  importBuiltinStations();
  const bool newsStationsAdded = importNewsStationPack();
  enrichStationIcons();
  sortStationsByRegionOnce(newsStationsAdded);
  loadUiTheme();
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
  const char *lcdStation = stationCount > 0 ? stations[selectedStation].name : "NETWORK RADIO";
  lcd.showStatus(lcdStation, WiFi.status() == WL_CONNECTED, WiFi.RSSI(),
                 playerVolume, audio.isRunning());
  lcd.update();
}
#endif  // NETWORK_RADIO_V8_NO_ENTRYPOINT
