/*
 * Network Radio 4.8.2 standalone Arduino sketch.
 * Project-local source dependencies are inlined in this file.
 *
 * The superseded V4 test-tone/I2S path and unused legacy web pages have
 * been removed. The active player, administration UI, OTA, recovery,
 * playlist, and WS2812 status paths are retained.
 */

/* Network Radio 3.0: player UI, administration UI, and WS2812B status LED. */

#define NETWORK_RADIO_VERSION "4.8.2"
#define NETWORK_RADIO_MAX_STATIONS 512
#ifdef NETWORK_RADIO_NO_ENTRYPOINT
#define NETWORK_RADIO_V8_NO_ENTRYPOINT
#endif

// Arduino's automatic prototype generator can misplace the entry-point
// declarations inside the large embedded HTML strings below. Declare them
// explicitly so setup() cannot be turned into a recursive call at build time.
#ifndef NETWORK_RADIO_V8_NO_ENTRYPOINT
void setup();
void loop();
#endif

// BEGIN INLINED: playback module
/* Network Radio V5: V4 playlist management plus HLS/AAC playback. */

#include <Audio.h>
#include <LittleFS.h>
#include <esp_private/periph_ctrl.h>
#include <esp32-hal-psram.h>

void audio_process_i2s(int32_t *outBuff, int16_t validSamples,
                       bool *continueI2S);

volatile uint32_t audioOutputPeak = 0;

// Apply a fixed +3 dB digital preamp after the user's volume setting and
// immediately before I2S output. Saturation prevents signed overflow and
// hard-clips peaks that have no remaining headroom.
void audio_process_i2s(int32_t *outBuff, int16_t validSamples,
                       bool *continueI2S) {
  (void)continueI2S;
  constexpr int32_t kPreampGainQ15 = 46286;  // 10^(3/20) * 2^15
  uint32_t blockPeak = 0;
  for (int16_t i = 0; i < validSamples; ++i) {
    const int64_t amplified =
        (static_cast<int64_t>(outBuff[i]) * kPreampGainQ15) >> 15;
    if (amplified > INT32_MAX) {
      outBuff[i] = INT32_MAX;
    } else if (amplified < INT32_MIN) {
      outBuff[i] = INT32_MIN;
    } else {
      outBuff[i] = static_cast<int32_t>(amplified);
    }
    const int64_t sample = outBuff[i];
    const uint32_t magnitude = static_cast<uint32_t>(
        sample < 0 ? -sample : sample);
    if (magnitude > blockPeak) blockPeak = magnitude;
  }
  if (blockPeak > audioOutputPeak) audioOutputPeak = blockPeak;
}

// Reuse V4's provisioning and playlist persistence helpers.
#ifndef NETWORK_RADIO_VERSION
#define NETWORK_RADIO_VERSION "0.5.0-playback"
#endif
// BEGIN INLINED: playlist and provisioning module
/*
 * Network Radio V4 - Wi-Fi provisioning + persistent playlist management.
 * Hardware output: ESP32-S3 GPIO4/5/6 -> MAX98357A BCLK/LRCLK/DIN.
 */

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <stdarg.h>

namespace config {
#ifndef NETWORK_RADIO_VERSION
#define NETWORK_RADIO_VERSION "4.8.2"
#endif
constexpr char kFirmwareVersion[] = NETWORK_RADIO_VERSION;
constexpr uint32_t kSerialBaud = 115200;
constexpr gpio_num_t kI2sBclk = GPIO_NUM_4;
constexpr gpio_num_t kI2sLrclk = GPIO_NUM_5;
constexpr gpio_num_t kI2sDataOut = GPIO_NUM_6;
constexpr uint8_t kPreviousTouchPin = 1;  // ESP32-S3 Touch1
constexpr uint8_t kNextTouchPin = 2;      // ESP32-S3 Touch2
constexpr uint8_t kPlayPauseTouchPin = 3; // ESP32-S3 Touch3
constexpr uint32_t kTouchScanIntervalMs = 25;
constexpr uint32_t kTouchDebugIntervalMs = 500;
constexpr uint8_t kTouchCalibrationSamples = 32;
constexpr uint8_t kTouchDebounceSamples = 3;
constexpr uint8_t kDefaultTouchSensitivityPercent = 8;
constexpr uint8_t kMinimumTouchSensitivityPercent = 3;
constexpr uint8_t kMaximumTouchSensitivityPercent = 50;
constexpr size_t kSerialCommandBufferSize = 48;

constexpr char kWifiNamespace[] = "radio";
constexpr char kSerialLogNamespace[] = "seriallog";
constexpr char kSerialLogMaskKey[] = "mask";
// Legacy single-network keys are retained for one-time migration.
constexpr char kWifiSsidKey[] = "wifi_ssid";
constexpr char kWifiPasswordKey[] = "wifi_pass";
constexpr char kWifiCountKey[] = "wifi_count";
constexpr uint8_t kMaxSavedWifiNetworks = 5;
constexpr size_t kWifiSsidSize = 33;
constexpr size_t kWifiPasswordSize = 65;
constexpr char kPlaylistNamespace[] = "playlist";
constexpr char kPlaylistCountKey[] = "count";
constexpr char kPlaylistSelectedKey[] = "selected";
constexpr char kPlaylistSelected16Key[] = "selected16";
constexpr char kPlaylistFavoriteContextKey[] = "favorite_ctx";
constexpr char kPlaylistSequenceKey[] = "sequence";
constexpr char kPlaylistBackendKey[] = "backend";
constexpr uint8_t kPlaylistBackendLegacyNvs = 1;
#ifndef NETWORK_RADIO_MAX_STATIONS
#define NETWORK_RADIO_MAX_STATIONS 16
#endif
constexpr uint16_t kMaxStations = NETWORK_RADIO_MAX_STATIONS;
static_assert(NETWORK_RADIO_MAX_STATIONS <= UINT16_MAX,
              "Station capacity must fit the persistent 16-bit index");
constexpr size_t kStationNameSize = 49;
constexpr size_t kStationUrlSize = 257;
constexpr size_t kStationLogoSize = 41;

constexpr char kAccessPointPrefix[] = "Radio-";
constexpr char kAccessPointPassword[] = "radio-setup";
constexpr uint8_t kAccessPointChannel = 6;
constexpr bool kKeepSetupAccessPointAvailable = true;
constexpr uint32_t kStationConnectTimeoutMs = 15000;
constexpr uint32_t kWifiConnectAttemptTimeoutMs = 8000;
constexpr uint32_t kStationRecoveryTimeoutMs = 20000;
constexpr char kMdnsName[] = "network-radio";
}  // namespace config

namespace {

struct Station {
  char name[config::kStationNameSize] = {};
  char url[config::kStationUrlSize] = {};
  char logo[config::kStationLogoSize] = {};
};

struct WifiNetwork {
  char ssid[config::kWifiSsidSize] = {};
  char password[config::kWifiPasswordSize] = {};
};

WebServer server(80);
DNSServer dnsServer;
Preferences preferences;
constexpr uint8_t kSerialLogSystemBit = 1U << 0;
constexpr uint8_t kSerialLogWifiBit = 1U << 1;
constexpr uint8_t kSerialLogAudioBit = 1U << 2;
constexpr uint8_t kSerialLogTouchBit = 1U << 3;
constexpr uint8_t kSerialLogAllBits = kSerialLogSystemBit | kSerialLogWifiBit |
                                      kSerialLogAudioBit | kSerialLogTouchBit;
uint8_t serialLogMask = kSerialLogSystemBit;
bool touchDebugEnabled = false;

bool serialLogEnabled(uint8_t category) {
  return (serialLogMask & category) != 0;
}

void serialLogPrintln(uint8_t category, const char *message) {
  if (serialLogEnabled(category)) Serial.println(message);
}

void serialLogPrintf(uint8_t category, const char *format, ...) {
  if (!serialLogEnabled(category)) return;
  va_list arguments;
  va_start(arguments, format);
  Serial.vprintf(format, arguments);
  va_end(arguments);
}

bool serialLogKindEnabled(const char *kind) {
  if (strcmp(kind, "wifi") == 0) return serialLogEnabled(kSerialLogWifiBit);
  if (strcmp(kind, "audio") == 0 || strcmp(kind, "player") == 0 ||
      strcmp(kind, "recovery") == 0) {
    return serialLogEnabled(kSerialLogAudioBit);
  }
  if (strcmp(kind, "touch") == 0) return serialLogEnabled(kSerialLogTouchBit);
  return serialLogEnabled(kSerialLogSystemBit);
}
Station *stations = nullptr;
uint16_t stationCount = 0;
uint16_t selectedStation = 0;
bool playlistStorageReady = false;
bool playlistFileStoreUnavailable = false;
bool legacyPlaylistMigrationPending = false;
bool playlistFormatMigrationPending = false;
uint8_t playlistActiveSlot = 0;
uint32_t playlistSequence = 0;
uint32_t playlistRevision = 1;

constexpr uint8_t kOtherStationGroupId = 250;
constexpr uint8_t kFavoriteStationGroupId = 255;
constexpr char kFavoriteStationsKey[] = "favorites_v1";
constexpr uint8_t kDefaultStationGroupOrder[] = {
  0, 1, 2, 3, 4, 5, 6,
  20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34,
  35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,
  kOtherStationGroupId,
};
constexpr uint8_t kStationGroupCapacity = sizeof(kDefaultStationGroupOrder);
uint8_t stationGroupOrder[kStationGroupCapacity] = {};
uint8_t stationGroupOrderCount = 0;
bool stationGroupOrderLoaded = false;
uint32_t favoriteStationKeys[config::kMaxStations] = {};
uint16_t favoriteStationCount = 0;
bool favoritePlaybackContext = false;

char accessPointSsid[20] = {};
bool accessPointRunning = false;
bool stationConfigured = false;
bool wifiCredentialsPresent = false;
WifiNetwork savedWifiNetworks[config::kMaxSavedWifiNetworks] = {};
uint8_t savedWifiNetworkCount = 0;
bool wifiScanInProgress = false;
enum class WifiRecoveryState : uint8_t { Idle, Scanning, Connecting };
WifiRecoveryState wifiRecoveryState = WifiRecoveryState::Idle;
int wifiRecoveryCandidates[config::kMaxSavedWifiNetworks] = {};
uint8_t wifiRecoveryCandidateCount = 0;
uint8_t wifiRecoveryCandidateIndex = 0;
uint32_t wifiRecoveryAttemptStartedAt = 0;
bool mdnsRunning = false;
void (*onStationSelected)(uint16_t) = nullptr;

bool requireAdmin();
uint8_t stationRegionOrder(const char *name);
const char *stationGroupName(const char *name);
const char *stationGroupNameById(uint8_t id);
bool regroupStationsInMemory();
bool persistStationGroupOrder();
bool persistSelectedStation();
void loadFavoriteStations();

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

String stationKey(uint16_t index) {
  return "item_" + String(index);
}

constexpr char kPlaylistSlotA[] = "/playlist_a.bin";
constexpr char kPlaylistSlotB[] = "/playlist_b.bin";
constexpr uint32_t kPlaylistMagic = 0x3150524EU;  // "NRP1"
constexpr uint16_t kPlaylistFormatVersion = 2;

struct PlaylistFileHeaderV1 {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  uint8_t selected;
  uint8_t reserved[3];
  uint32_t sequence;
  uint32_t checksum;
};

struct PlaylistFileHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  uint16_t selected;
  uint16_t reserved;
  uint32_t sequence;
  uint32_t checksum;
};

static_assert(sizeof(PlaylistFileHeaderV1) == sizeof(PlaylistFileHeader),
              "Playlist header versions must retain their on-flash size");

static_assert(sizeof(Station) == config::kStationNameSize +
                                  config::kStationUrlSize +
                                  config::kStationLogoSize,
              "Station must remain a packed character record for playlist storage");

bool allocateStationStore() {
  if (stations != nullptr) return true;
  if (!psramFound() && !psramInit()) {
    serialLogPrintln(kSerialLogSystemBit,
                     "ERROR: PSRAM is required for the station store.");
    return false;
  }
  stations = static_cast<Station *>(
      ps_calloc(config::kMaxStations, sizeof(Station)));
  if (stations == nullptr) {
    serialLogPrintln(kSerialLogSystemBit,
                     "ERROR: Could not allocate the station store in PSRAM.");
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

uint32_t stationFavoriteKey(const Station &station) {
  return playlistChecksumUpdate(2166136261UL, station.url,
                                strlen(station.url));
}

int favoriteStationIndexForKey(uint32_t key) {
  for (uint16_t index = 0; index < stationCount; ++index) {
    if (stationFavoriteKey(stations[index]) == key) return index;
  }
  return -1;
}

int favoriteStationPosition(uint16_t stationIndex) {
  const uint32_t key = stationFavoriteKey(stations[stationIndex]);
  for (uint16_t index = 0; index < favoriteStationCount; ++index) {
    if (favoriteStationKeys[index] == key) return index;
  }
  return -1;
}

uint16_t adjacentStationIndex(bool previous) {
  const int favoritePosition = favoriteStationPosition(selectedStation);
  if (favoritePlaybackContext && favoritePosition >= 0 &&
      favoriteStationCount > 0) {
    const uint16_t position = static_cast<uint16_t>(favoritePosition);
    const uint16_t targetPosition = previous
        ? (position == 0 ? favoriteStationCount - 1 : position - 1)
        : (position + 1) % favoriteStationCount;
    const int target =
        favoriteStationIndexForKey(favoriteStationKeys[targetPosition]);
    if (target >= 0) return static_cast<uint16_t>(target);
  }
  return previous
      ? (selectedStation == 0 ? stationCount - 1 : selectedStation - 1)
      : (selectedStation + 1) % stationCount;
}

bool persistFavoriteStations() {
  Preferences catalogPreferences;
  if (!catalogPreferences.begin("catalog", false)) return false;
  bool saved = true;
  if (favoriteStationCount == 0) {
    if (catalogPreferences.isKey(kFavoriteStationsKey)) {
      saved = catalogPreferences.remove(kFavoriteStationsKey);
    }
  } else {
    const size_t length = favoriteStationCount * sizeof(uint32_t);
    saved = catalogPreferences.putBytes(kFavoriteStationsKey,
                                         favoriteStationKeys, length) == length;
  }
  catalogPreferences.end();
  return saved;
}

void loadFavoriteStations() {
  favoriteStationCount = 0;
  Preferences catalogPreferences;
  if (!catalogPreferences.begin("catalog", true)) return;
  const size_t storedLength =
      catalogPreferences.getBytesLength(kFavoriteStationsKey);
  const size_t readLength =
      storedLength > 0 && storedLength <= sizeof(favoriteStationKeys) &&
              storedLength % sizeof(uint32_t) == 0
          ? catalogPreferences.getBytes(kFavoriteStationsKey,
                                        favoriteStationKeys, storedLength)
          : 0;
  catalogPreferences.end();
  favoriteStationCount = readLength / sizeof(uint32_t);

  uint16_t output = 0;
  for (uint16_t input = 0; input < favoriteStationCount; ++input) {
    if (favoriteStationIndexForKey(favoriteStationKeys[input]) < 0) continue;
    bool duplicate = false;
    for (uint16_t prior = 0; prior < output; ++prior) {
      if (favoriteStationKeys[prior] == favoriteStationKeys[input]) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) favoriteStationKeys[output++] = favoriteStationKeys[input];
  }
  if (output != favoriteStationCount) {
    favoriteStationCount = output;
    persistFavoriteStations();
  }
}

void loadFavoritePlaybackContext() {
  favoritePlaybackContext = false;
  if (!preferences.begin(config::kPlaylistNamespace, true)) return;
  favoritePlaybackContext =
      preferences.getBool(config::kPlaylistFavoriteContextKey, false);
  preferences.end();
}

void validateFavoritePlaybackContext() {
  if (!favoritePlaybackContext) return;
  if (stationCount > 0 && favoriteStationPosition(selectedStation) >= 0) return;
  favoritePlaybackContext = false;
  persistSelectedStation();
}

uint32_t playlistChecksum(uint16_t count, uint16_t selected,
                          const Station *entries) {
  uint32_t value = 2166136261UL;
  value = playlistChecksumUpdate(value, &count, sizeof(count));
  value = playlistChecksumUpdate(value, &selected, sizeof(selected));
  return playlistChecksumUpdate(value, entries, count * sizeof(Station));
}

uint32_t playlistChecksumV1(uint16_t count, uint8_t selected,
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
      (header.version != 1 && header.version != kPlaylistFormatVersion) ||
      header.count == 0 ||
      header.count > config::kMaxStations || header.selected >= header.count ||
      file.size() != sizeof(header) + header.count * sizeof(Station)) {
    file.close();
    return false;
  }

  uint32_t checksum = 2166136261UL;
  checksum = playlistChecksumUpdate(checksum, &header.count, sizeof(header.count));
  if (header.version == 1) {
    const uint8_t legacySelection = static_cast<uint8_t>(header.selected);
    checksum = playlistChecksumUpdate(checksum, &legacySelection,
                                      sizeof(legacySelection));
  } else {
    checksum = playlistChecksumUpdate(checksum, &header.selected,
                                      sizeof(header.selected));
  }
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
  stationCount = header.count;
  selectedStation = header.selected;
  playlistSequence = header.sequence;
  playlistFormatMigrationPending = header.version != kPlaylistFormatVersion;
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
    const uint16_t storedSelection = preferences.getUShort(
        config::kPlaylistSelected16Key,
        preferences.getUChar(config::kPlaylistSelectedKey,
                             static_cast<uint8_t>(min<uint16_t>(selectedStation, 255))));
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
  serialLogPrintf(kSerialLogSystemBit,
                  "WARN: Playlist snapshot %s %s (LittleFS %u/%u bytes).\n",
                  path, stage, static_cast<unsigned>(LittleFS.usedBytes()),
                  static_cast<unsigned>(LittleFS.totalBytes()));
}

bool writePlaylistFile(const char *path, uint32_t sequence) {
  PlaylistFileHeader header = {
      kPlaylistMagic,
      kPlaylistFormatVersion,
      stationCount,
      selectedStation,
      0,
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
  for (uint16_t index = 0; index < stationCount; ++index) {
    if (file.write(reinterpret_cast<const uint8_t *>(&stations[index]),
                   sizeof(Station)) == sizeof(Station)) {
      continue;
    }
    file.close();
    serialLogPrintf(kSerialLogSystemBit,
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
                     preferences.putUShort(config::kPlaylistSelected16Key,
                                            selectedStation) == sizeof(uint16_t) &&
                     preferences.putBool(config::kPlaylistFavoriteContextKey,
                                         favoritePlaybackContext) == 1 &&
                     preferences.putUInt(config::kPlaylistSequenceKey,
                                         playlistSequence) == sizeof(uint32_t);
  preferences.end();
  if (saved) legacyPlaylistMigrationPending = false;
  return saved;
}

bool persistLegacyPlaylist() {
  if (stationCount > 255 || selectedStation > 255) return false;
  if (!preferences.begin(config::kPlaylistNamespace, false)) return false;
  bool saved = preferences.clear() &&
               preferences.putUChar(config::kPlaylistCountKey,
                                    static_cast<uint8_t>(stationCount)) == 1 &&
               preferences.putUChar(config::kPlaylistSelectedKey,
                                    static_cast<uint8_t>(selectedStation)) == 1 &&
               preferences.putBool(config::kPlaylistFavoriteContextKey,
                                   favoritePlaybackContext) == 1;
  for (uint16_t index = 0; saved && index < stationCount; ++index) {
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
  const bool saved = preferences.putUShort(config::kPlaylistSelected16Key,
                                            selectedStation) == sizeof(uint16_t) &&
                     preferences.putBool(config::kPlaylistFavoriteContextKey,
                                         favoritePlaybackContext) == 1 &&
                     preferences.putUInt(config::kPlaylistSequenceKey,
                                         playlistSequence) == sizeof(uint32_t);
  preferences.end();
  return saved;
}

bool persistSelectedStation() {
  // Selecting a station writes only the tiny state record.  The two-slot
  // playlist snapshot is reserved for structural changes, avoiding a full
  // full playlist rewrite for every tap on the player UI.
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
        serialLogPrintln(kSerialLogSystemBit,
                         "WARN: Legacy playlist has been kept as a recovery copy.");
      } else if (!persistPlaylistState()) {
        serialLogPrintln(kSerialLogSystemBit,
                         "WARN: Could not persist playlist selection state.");
      }
    }
    if (!saved) {
      playlistFileStoreUnavailable = true;
      serialLogPrintln(kSerialLogSystemBit,
          "WARN: LittleFS playlist snapshots are unavailable; subsequent "
          "playlist changes will use legacy NVS storage.");
    }
  }
  if (!saved) {
    if (preserveLoadedLegacy) {
      if (!markLegacyPlaylistStorePreferred()) {
        serialLogPrintln(kSerialLogSystemBit,
                         "WARN: Could not mark the legacy playlist as preferred.");
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
  stationCount = min<uint16_t>(storedCount, config::kMaxStations);
  selectedStation = preferences.getUShort(
      config::kPlaylistSelected16Key,
      preferences.getUChar(config::kPlaylistSelectedKey, 0));
  for (uint16_t index = 0; index < stationCount; ++index) {
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
      serialLogPrintln(kSerialLogSystemBit,
          "WARN: Using the preserved legacy NVS playlist after a prior "
          "LittleFS write failure.");
      return;
    }
    if (!persistPlaylist(true)) {
      serialLogPrintln(kSerialLogSystemBit,
          "WARN: LittleFS playlist storage unavailable; legacy NVS playlist "
          "retained.");
    }
    return;
  }

  if (preferLegacy && loadPlaylistFromFiles()) {
    serialLogPrintln(kSerialLogSystemBit,
                     "WARN: Legacy NVS playlist unavailable; recovered from LittleFS.");
    return;
  }

  setDefaultPlaylist();
  if (!persistPlaylist()) {
    serialLogPrintln(kSerialLogSystemBit,
                     "ERROR: Could not persist the playlist.");
  }
}

void sendPlaylistJson(bool includeUrls, int statusCode = 200) {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(statusCode, "application/json; charset=utf-8", "");
  uint16_t groupCounts[256] = {};
  for (uint16_t index = 0; index < stationCount; ++index) {
    ++groupCounts[stationRegionOrder(stations[index].name)];
  }

  String chunk = "{\"revision\":" + String(playlistRevision) +
                 ",\"selected\":" + String(selectedStation) +
                 ",\"groups\":[";
  chunk.reserve(1024);
  chunk += "{\"id\":" + String(kFavoriteStationGroupId) +
           ",\"name\":\"收藏\",\"count\":" +
           String(favoriteStationCount) + "}";
  bool firstGroup = false;
  for (uint8_t order = 0; order < stationGroupOrderCount; ++order) {
    const uint8_t groupId = stationGroupOrder[order];
    if (groupCounts[groupId] == 0) continue;
    if (!firstGroup) chunk += ',';
    firstGroup = false;
    chunk += "{\"id\":" + String(groupId) + ",\"name\":\"" +
             jsonEscape(stationGroupNameById(groupId)) + "\",\"count\":" +
             String(groupCounts[groupId]) + "}";
    if (chunk.length() >= 768) {
      server.sendContent(chunk);
      chunk = "";
    }
  }
  chunk += "],\"stations\":[";
  for (uint16_t index = 0; index < stationCount; ++index) {
    if (index > 0) chunk += ',';
    const uint8_t groupId = stationRegionOrder(stations[index].name);
    chunk += "{\"id\":" + String(index) + ",\"name\":\"" +
             jsonEscape(stations[index].name) + "\"";
    if (includeUrls) {
      chunk += ",\"url\":\"" + jsonEscape(stations[index].url) + "\"";
    }
    chunk += ",\"logo\":\"" + jsonEscape(stations[index].logo) +
             "\",\"group_id\":" + String(groupId) +
             ",\"group\":\"" + jsonEscape(stationGroupNameById(groupId)) +
             "\"}";
    if (chunk.length() >= 768) {
      server.sendContent(chunk);
      chunk = "";
    }
  }
  chunk += "],\"favorites\":[";
  bool firstFavorite = true;
  for (uint16_t position = 0; position < favoriteStationCount; ++position) {
    const int stationIndex =
        favoriteStationIndexForKey(favoriteStationKeys[position]);
    if (stationIndex < 0) continue;
    if (!firstFavorite) chunk += ',';
    firstFavorite = false;
    chunk += String(stationIndex);
  }
  chunk += "]}";
  server.sendContent(chunk);
  server.sendContent("");
}

void startAccessPoint() {
  if (accessPointRunning) {
    return;
  }
  WiFi.mode(stationConfigured ? WIFI_AP_STA : WIFI_AP);
  if (!WiFi.softAP(accessPointSsid, config::kAccessPointPassword,
                   config::kAccessPointChannel, false, 4)) {
    serialLogPrintln(kSerialLogWifiBit,
                     "ERROR: Wi-Fi access point failed to start.");
    return;
  }
  dnsServer.start(53, "*", WiFi.softAPIP());
  accessPointRunning = true;
  serialLogPrintf(kSerialLogWifiBit, "Setup AP: %s / http://%s\n",
                  accessPointSsid, WiFi.softAPIP().toString().c_str());
}

void startMdns() {
  if (!mdnsRunning && MDNS.begin(config::kMdnsName)) {
    MDNS.addService("http", "tcp", 80);
    mdnsRunning = true;
  }
}

String wifiSsidKey(uint8_t index) {
  return "wifi_ssid_" + String(index);
}

String wifiPasswordKey(uint8_t index) {
  return "wifi_pass_" + String(index);
}

bool persistSavedWifiNetworks() {
  if (!preferences.begin(config::kWifiNamespace, false)) return false;
  bool saved = preferences.putUChar(config::kWifiCountKey, savedWifiNetworkCount) ==
               sizeof(savedWifiNetworkCount);
  for (uint8_t index = 0; index < savedWifiNetworkCount; ++index) {
    saved = preferences.putString(wifiSsidKey(index).c_str(),
                                  savedWifiNetworks[index].ssid) ==
                strlen(savedWifiNetworks[index].ssid) &&
            saved;
    saved = preferences.putString(wifiPasswordKey(index).c_str(),
                                  savedWifiNetworks[index].password) ==
                strlen(savedWifiNetworks[index].password) &&
            saved;
  }
  for (uint8_t index = savedWifiNetworkCount;
       index < config::kMaxSavedWifiNetworks; ++index) {
    preferences.remove(wifiSsidKey(index).c_str());
    preferences.remove(wifiPasswordKey(index).c_str());
  }
  // Remove the legacy values only after the new list has been written.
  if (saved) {
    preferences.remove(config::kWifiSsidKey);
    preferences.remove(config::kWifiPasswordKey);
  }
  preferences.end();
  return saved;
}

bool loadSavedWifiNetworks() {
  savedWifiNetworkCount = 0;
  memset(savedWifiNetworks, 0, sizeof(savedWifiNetworks));
  if (!preferences.begin(config::kWifiNamespace, true)) return false;

  const uint8_t storedCount = min<uint8_t>(
      preferences.getUChar(config::kWifiCountKey, 0),
      config::kMaxSavedWifiNetworks);
  for (uint8_t index = 0; index < storedCount; ++index) {
    const String ssid = preferences.getString(wifiSsidKey(index).c_str(), "");
    const String password = preferences.getString(wifiPasswordKey(index).c_str(), "");
    if (ssid.isEmpty() || ssid.length() >= config::kWifiSsidSize ||
        password.length() >= config::kWifiPasswordSize) {
      continue;
    }
    strlcpy(savedWifiNetworks[savedWifiNetworkCount].ssid, ssid.c_str(),
            config::kWifiSsidSize);
    strlcpy(savedWifiNetworks[savedWifiNetworkCount].password, password.c_str(),
            config::kWifiPasswordSize);
    ++savedWifiNetworkCount;
  }

  const String legacySsid = preferences.getString(config::kWifiSsidKey, "");
  const String legacyPassword = preferences.getString(config::kWifiPasswordKey, "");
  preferences.end();

  // Versions before 4.1.0 stored a single network. Preserve it automatically.
  if (savedWifiNetworkCount == 0 && !legacySsid.isEmpty() &&
      legacySsid.length() < config::kWifiSsidSize &&
      legacyPassword.length() < config::kWifiPasswordSize) {
    strlcpy(savedWifiNetworks[0].ssid, legacySsid.c_str(), config::kWifiSsidSize);
    strlcpy(savedWifiNetworks[0].password, legacyPassword.c_str(),
            config::kWifiPasswordSize);
    savedWifiNetworkCount = 1;
    if (!persistSavedWifiNetworks()) {
      serialLogPrintln(kSerialLogWifiBit,
                       "WARN: Could not migrate legacy Wi-Fi configuration.");
    }
  }
  return true;
}

String savedWifiNetworksJson() {
  loadSavedWifiNetworks();
  String json = "{\"saved\":[";
  for (uint8_t index = 0; index < savedWifiNetworkCount; ++index) {
    if (index) json += ',';
    json += "{\"ssid\":\"" + jsonEscape(savedWifiNetworks[index].ssid) + "\"}";
  }
  json += "],\"connected_ssid\":\"" +
          jsonEscape(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "") + "\"}";
  return json;
}

bool connectWifiNetwork(uint8_t index) {
  serialLogPrintf(kSerialLogWifiBit, "Wi-Fi: trying saved network %s\n",
                  savedWifiNetworks[index].ssid);
  WiFi.begin(savedWifiNetworks[index].ssid, savedWifiNetworks[index].password);
  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startedAt < config::kWifiConnectAttemptTimeoutMs) {
    delay(250);
  }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(false, false);
    return false;
  }
  serialLogPrintf(kSerialLogWifiBit, "Wi-Fi: connected to %s\n",
                  savedWifiNetworks[index].ssid);
  WiFi.setAutoReconnect(true);
  startMdns();
  return true;
}

bool connectSavedStation() {
  if (!loadSavedWifiNetworks()) {
    serialLogPrintln(kSerialLogWifiBit,
                     "ERROR: Could not read saved Wi-Fi credentials.");
    return false;
  }
  wifiCredentialsPresent = savedWifiNetworkCount > 0;
  stationConfigured = wifiCredentialsPresent;
  if (!wifiCredentialsPresent) return false;

  WiFi.mode(accessPointRunning ? WIFI_AP_STA : WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  delay(100);

  int candidates[config::kMaxSavedWifiNetworks];
  uint8_t candidateCount = 0;
  bool selected[config::kMaxSavedWifiNetworks] = {};
  const int scanCount = WiFi.scanNetworks(false, true);
  if (scanCount >= 0) {
    // Prefer the strongest visible saved network, while still falling back to
    // the other visible choices if its router rejects the connection.
    for (uint8_t rank = 0; rank < savedWifiNetworkCount; ++rank) {
      int best = -1;
      int32_t bestRssi = -127;
      for (uint8_t saved = 0; saved < savedWifiNetworkCount; ++saved) {
        if (selected[saved]) continue;
        for (int found = 0; found < scanCount; ++found) {
          if (WiFi.SSID(found) == savedWifiNetworks[saved].ssid &&
              WiFi.RSSI(found) > bestRssi) {
            best = saved;
            bestRssi = WiFi.RSSI(found);
          }
        }
      }
      if (best < 0) break;
      selected[best] = true;
      candidates[candidateCount++] = best;
    }
  }
  WiFi.scanDelete();

  // Also try SSIDs hidden from scanning, in saved order.
  for (uint8_t index = 0; index < savedWifiNetworkCount; ++index) {
    if (!selected[index]) candidates[candidateCount++] = index;
  }
  for (uint8_t candidate = 0; candidate < candidateCount; ++candidate) {
    if (connectWifiNetwork(candidates[candidate])) return true;
  }
  return false;
}

void handleWifiScan() {
  if (wifiRecoveryState != WifiRecoveryState::Idle) {
    sendJson("{\"error\":\"正在自动选择已保存的 Wi-Fi，请稍后再扫描\"}", 409);
    return;
  }
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
      serialLogPrintf(kSerialLogWifiBit,
                      "ERROR: Wi-Fi scan could not start (mode=%d, status=%d).\n",
                      static_cast<int>(WiFi.getMode()),
                      static_cast<int>(WiFi.status()));
      sendJson("{\"error\":\"Wi-Fi scan could not start\"}", 503);
      return;
    }
    wifiScanInProgress = true;
    serialLogPrintln(kSerialLogWifiBit, "Wi-Fi scan started.");
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
    serialLogPrintf(kSerialLogWifiBit,
                    "ERROR: Wi-Fi scan failed (result=%d, mode=%d, status=%d).\n",
                    count, static_cast<int>(WiFi.getMode()),
                    static_cast<int>(WiFi.status()));
    WiFi.scanDelete();
    sendJson("{\"error\":\"Wi-Fi scan failed\"}", 500);
    return;
  }
  serialLogPrintf(kSerialLogWifiBit,
                  "Wi-Fi scan completed: %d network(s).\n", count);
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
  if (!loadSavedWifiNetworks()) {
    sendJson("{\"error\":\"could not read Wi-Fi settings\"}", 500);
    return;
  }
  int existing = -1;
  for (uint8_t index = 0; index < savedWifiNetworkCount; ++index) {
    if (ssid == savedWifiNetworks[index].ssid) {
      existing = index;
      break;
    }
  }
  if (existing < 0 && savedWifiNetworkCount >= config::kMaxSavedWifiNetworks) {
    sendJson("{\"error\":\"最多保存 5 个 Wi-Fi；请先删除一个网络\"}", 409);
    return;
  }
  const uint8_t target = existing >= 0 ? static_cast<uint8_t>(existing)
                                       : savedWifiNetworkCount++;
  strlcpy(savedWifiNetworks[target].ssid, ssid.c_str(), config::kWifiSsidSize);
  strlcpy(savedWifiNetworks[target].password, password.c_str(),
          config::kWifiPasswordSize);
  if (!persistSavedWifiNetworks()) {
    sendJson("{\"error\":\"could not save Wi-Fi settings\"}", 500);
    return;
  }
  sendJson("{\"saved\":true,\"restarting\":true,\"networks\":" +
           savedWifiNetworksJson() + "}");
  delay(300);
  ESP.restart();
}

void handleDeleteWifi() {
  if (!requireAdmin()) return;
  const String ssid = server.arg("ssid");
  if (ssid.isEmpty() || !loadSavedWifiNetworks()) {
    sendJson("{\"error\":\"Wi-Fi network not found\"}", 404);
    return;
  }
  int found = -1;
  for (uint8_t index = 0; index < savedWifiNetworkCount; ++index) {
    if (ssid == savedWifiNetworks[index].ssid) {
      found = index;
      break;
    }
  }
  if (found < 0) {
    sendJson("{\"error\":\"Wi-Fi network not found\"}", 404);
    return;
  }
  for (uint8_t index = static_cast<uint8_t>(found);
       index + 1 < savedWifiNetworkCount; ++index) {
    savedWifiNetworks[index] = savedWifiNetworks[index + 1];
  }
  --savedWifiNetworkCount;
  memset(&savedWifiNetworks[savedWifiNetworkCount], 0, sizeof(WifiNetwork));
  if (!persistSavedWifiNetworks()) {
    sendJson("{\"error\":\"could not save Wi-Fi settings\"}", 500);
    return;
  }
  sendJson("{\"deleted\":true,\"restarting\":true}");
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

bool parseStationId(uint16_t &id) {
  if (!server.hasArg("id")) return false;
  const String value = server.arg("id");
  if (value.isEmpty()) return false;
  uint32_t parsed = 0;
  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value[index];
    if (character < '0' || character > '9') return false;
    parsed = parsed * 10U + static_cast<uint8_t>(character - '0');
    if (parsed >= stationCount || parsed > UINT16_MAX) return false;
  }
  id = static_cast<uint16_t>(parsed);
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
  const uint16_t previousCount = stationCount;
  const uint16_t previousSelection = selectedStation;
  Station *previousStations = previousCount == 0 ? nullptr :
      static_cast<Station *>(ps_malloc(previousCount * sizeof(Station)));
  if (previousCount > 0 && previousStations == nullptr) {
    sendJson("{\"error\":\"not enough memory to update playlist\"}", 500);
    return;
  }
  if (previousStations != nullptr) {
    memcpy(previousStations, stations, previousCount * sizeof(Station));
  }
  stations[stationCount] = Station{};
  strlcpy(stations[stationCount].name, name.c_str(), sizeof(stations[0].name));
  strlcpy(stations[stationCount].url, url.c_str(), sizeof(stations[0].url));
  ++stationCount;
  if (!regroupStationsInMemory() || !persistPlaylist()) {
    stationCount = previousCount;
    selectedStation = previousSelection;
    if (previousStations != nullptr) {
      memcpy(stations, previousStations, previousCount * sizeof(Station));
    }
    stations[stationCount] = Station{};
    free(previousStations);
    sendJson("{\"error\":\"could not save playlist\"}", 500);
    return;
  }
  free(previousStations);
  sendPlaylistJson(true, 201);
}

void handleUpdateStation() {
  uint16_t id;
  const String name = server.arg("name");
  const String url = server.arg("url");
  if (!parseStationId(id) || !validateStation(name, url)) {
    sendJson("{\"error\":\"invalid station data\"}", 400);
    return;
  }
  Station *previousStations = static_cast<Station *>(
      ps_malloc(stationCount * sizeof(Station)));
  if (previousStations == nullptr) {
    sendJson("{\"error\":\"not enough memory to update playlist\"}", 500);
    return;
  }
  memcpy(previousStations, stations, stationCount * sizeof(Station));
  const uint16_t previousSelection = selectedStation;
  const int favoritePosition = favoriteStationPosition(id);
  const uint32_t previousFavoriteKey =
      favoritePosition >= 0 ? favoriteStationKeys[favoritePosition] : 0;
  strlcpy(stations[id].name, name.c_str(), sizeof(stations[id].name));
  strlcpy(stations[id].url, url.c_str(), sizeof(stations[id].url));
  bool saved = regroupStationsInMemory() && persistPlaylist();
  if (saved && favoritePosition >= 0) {
    favoriteStationKeys[favoritePosition] =
        playlistChecksumUpdate(2166136261UL, url.c_str(), url.length());
    saved = persistFavoriteStations();
  }
  if (!saved) {
    memcpy(stations, previousStations, stationCount * sizeof(Station));
    selectedStation = previousSelection;
    if (favoritePosition >= 0) {
      favoriteStationKeys[favoritePosition] = previousFavoriteKey;
    }
    persistPlaylist();
  }
  free(previousStations);
  if (saved) sendPlaylistJson(true);
  else sendJson("{\"error\":\"could not save playlist\"}", 500);
}

void handleDeleteStation() {
  uint16_t id;
  if (!parseStationId(id) || stationCount <= 1) {
    sendJson("{\"error\":\"cannot delete this station\"}", 400);
    return;
  }
  const uint16_t previousSelection = selectedStation;
  const int favoritePosition = favoriteStationPosition(id);
  const Station removed = stations[id];
  for (uint16_t index = id; index + 1 < stationCount; ++index) {
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
    for (uint16_t index = stationCount; index > id; --index) {
      stations[index] = stations[index - 1];
    }
    stations[id] = removed;
    ++stationCount;
    selectedStation = previousSelection;
  }
  if (saved && favoritePosition >= 0) {
    for (uint16_t index = favoritePosition;
         index + 1 < favoriteStationCount; ++index) {
      favoriteStationKeys[index] = favoriteStationKeys[index + 1];
    }
    --favoriteStationCount;
    favoriteStationKeys[favoriteStationCount] = 0;
    if (!persistFavoriteStations()) {
      serialLogPrintln(kSerialLogSystemBit,
                       "WARN: Could not remove deleted station from favorites.");
    }
    if (id == previousSelection) {
      favoritePlaybackContext = false;
      if (!persistSelectedStation()) {
        serialLogPrintln(kSerialLogSystemBit,
                         "WARN: Could not clear favorite playback context.");
      }
    }
  }
  if (saved) sendPlaylistJson(true);
  else sendJson("{\"error\":\"could not save playlist\"}", 500);
}

void handleSelectStation() {
  uint16_t id;
  if (!parseStationId(id)) {
    sendJson("{\"error\":\"invalid station id\"}", 400);
    return;
  }
  const uint16_t previousSelection = selectedStation;
  const bool previousFavoriteContext = favoritePlaybackContext;
  selectedStation = id;
  favoritePlaybackContext = server.arg("context") == "favorites" &&
                            favoriteStationPosition(selectedStation) >= 0;
  const bool saved = persistSelectedStation();
  if (!saved) {
    selectedStation = previousSelection;
    favoritePlaybackContext = previousFavoriteContext;
  }
  if (saved && onStationSelected != nullptr) {
    onStationSelected(selectedStation);
  }
  if (saved) sendPlaylistJson(true);
  else sendJson("{\"error\":\"could not save playlist\"}", 500);
}

void handleMoveStation() {
  uint16_t id;
  const String direction = server.arg("direction");
  if (!parseStationId(id) ||
      (direction != "up" && direction != "down") ||
      (direction == "up" && id == 0) ||
      (direction == "down" && id + 1 >= stationCount)) {
    sendJson("{\"error\":\"cannot move station\"}", 400);
    return;
  }
  const uint16_t other = direction == "up" ? id - 1 : id + 1;
  const Station selected = stations[id];
  stations[id] = stations[other];
  stations[other] = selected;
  if (selectedStation == id) selectedStation = other;
  else if (selectedStation == other) selectedStation = id;
  const bool saved = persistPlaylist();
  if (saved) sendPlaylistJson(true);
  else sendJson("{\"error\":\"could not save playlist\"}", 500);
}

}  // namespace
// END INLINED: playlist and provisioning module

namespace {

Audio audio;
Preferences playerPreferences;
uint8_t playerVolume = 12;  // ESP32-audioI2S range: 0..21
char playerMessage[96] = "idle";
bool playerRequested = false;
bool audioOutputReady = false;

void setPlayerMessage(const char *message) {
  strlcpy(playerMessage, message ? message : "", sizeof(playerMessage));
}

void audioInfo(Audio::msg_t message) {
  if (message.msg != nullptr) setPlayerMessage(message.msg);
  if (message.s != nullptr && message.msg != nullptr) {
    serialLogPrintf(kSerialLogAudioBit, "audio %s: %s\n", message.s,
                    message.msg);
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

// END INLINED: playback module


// BEGIN INLINED: built-in station catalog
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
  {"北京新闻广播","https://satellitepull.cnr.cn/live/wxbjxwgb/playlist.m3u8"},
  {"北京文艺广播","http://ls.qingting.fm/live/333.m3u8"},
  {"北京城市广播","https://satellitepull.cnr.cn/live/wxbjcsfwgl/playlist.m3u8"},
  {"北京体育广播","https://brtv-radiolive.rbc.cn/alive/fm1025.m3u8"},
  {"北京阳光调频","https://lhttp.qtfm.cn/live/5021739/64k.mp3"},
  {"北京经典调频","https://radio.0472.org/?id=1254"},
  {"CRI华语环球","http://sk.cri.cn/hyhq.m3u8"},
  {"CRI环球资讯","https://sk.cri.cn/905.m3u8"},
  {"CRI南海之声","https://sk.cri.cn/nhzs.m3u8"},
  {"CRI英语资讯","http://sk.cri.cn/am846.m3u8"},
  {"RTHK3","https://rthkradio3-live.akamaized.net/hls/live/2040079/radio3/master.m3u8"},
  {"香港电台普通话台","https://rthkradiopth-live.akamaized.net/hls/live/2040082/radiopth/master.m3u8"},
  {"湖南经济广播","https://satellitepull.cnr.cn/live/wx32hunjjgb/playlist.m3u8"},
  {"湖南新闻频道","https://radio.0472.org/?id=525"},
  {"湖南潇湘之声","https://satellitepull.cnr.cn/live/wx32hunyygb/playlist.m3u8"},
  {"重庆文艺广播","https://satellitepull.cnr.cn/live/wxcqwygb/playlist.m3u8"},
  {"上海故事广播","http://live.cooltv.top/tv/news1296.php?id=10"},
  {"山西文艺广播","http://radiolive.sxrtv.com/live/wenyi/playlist.m3u8"},
  {"江苏文艺广播","https://satellitepull.cnr.cn/live/wx32jswygb/playlist.m3u8"},
  {"江苏故事广播","https://satellitepull.cnr.cn/live/wx32jsgsgb/playlist.m3u8"},
  {"上海戏曲广播","https://radio.0472.org/?id=1314"},
  {"陕西故事广播","https://radio.0472.org/?id=1133"},
  {"北京音乐广播","http://ls.qingting.fm/live/332.m3u8"},
  {"湖南金鹰之声","https://satellitepull.cnr.cn/live/wx32955/playlist.m3u8"},
  {"芒果时空音乐","https://radio.0472.org/?id=524"},
  {"湖南交通广播","https://satellitepull.cnr.cn/live/wx32hunjtgb/playlist.m3u8"},
  {"湖南音乐之声","https://radio.0472.org/?id=1060"},
  {"长沙交通广播","https://radio.0472.org/?id=1061"},
  {"长沙音乐广播","https://radio.0472.org/?id=1531"},
  {"广东音乐之声","https://satellitepull.cnr.cn/live/wxgdyyzs/playlist.m3u8"},
  {"江西音乐广播","https://satellitepull.cnr.cn/live/wx32jiangxyygb/playlist.m3u8"},
  {"河北音乐广播","https://satellitepull.cnr.cn/live/wxhebyygb/playlist.m3u8"},
  {"深圳音乐频率","https://radio.0472.org/?id=498"},
  {"南京音乐广播","http://hls.njgb.com/live_hls/4/playlist.m3u8"},
  {"江苏音乐广播","https://satellitepull.cnr.cn/live/wx32jsyygb/playlist.m3u8"},
  {"河北汽车音乐","https://radio.pull.hebtv.com/live/hebqcyy.m3u8"},
  {"重庆音乐广播","https://satellitepull.cnr.cn/live/wxcqyygb/playlist.m3u8"},
  {"龙江音乐广播","https://radio.0472.org/?id=552"},
  {"内蒙音乐之声","https://radio.0472.org/?id=572"},
  {"宁夏音乐广播","https://satellitepull.cnr.cn/live/wxnxyygb/playlist.m3u8"},
  {"陕西音乐广播","https://satellitepull.cnr.cn/live/wxsxxyygb/playlist.m3u8"},
  {"青海音乐广播","https://radio.0472.org/?id=616"},
  {"山西音乐广播","https://radio.0472.org/?id=637"},
  {"山东音乐广播","https://satellitepull.cnr.cn/live/wxsdyygb/playlist.m3u8"},
  {"安徽音乐广播","https://satellitepull.cnr.cn/live/wxahyygb/playlist.m3u8"},
  {"江苏经典流行","http://satellitepull.cnr.cn/live/wx32jsjdlxyy/playlist.m3u8"},
  {"浙江音乐调频","https://radio.0472.org/?id=928"},
  {"厦门音乐广播","https://radio.0472.org/?id=1897"},
  {"云南音乐广播","https://satellitepull.cnr.cn/live/wxynyygb/playlist.m3u8"},
  {"广西音乐台","https://radio.0472.org/?id=590"},
  {"贵州音乐广播","https://satellitepull.cnr.cn/live/wx32gzyygb/playlist.m3u8"},
  {"新疆音乐广播","https://radio.0472.org/?id=1158"},
  {"海南音乐广播","https://satellitepull.cnr.cn/live/wxhainyygb/playlist.m3u8"},
  {"河北文艺广播","https://satellitepull.cnr.cn/live/wxhebwygb/playlist.m3u8"},
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
  {"BCC中广流行","http://stream.rcs.revma.com/aw9uqyxy2tzuv"},
  {"BCC中广新闻","http://stream.rcs.revma.com/78fm9wyy2tzuv"},
  {"BCC中广音乐","http://stream.rcs.revma.com/ks4vsmg3qtzuv"},
  {"Capital FM","http://22893.live.streamtheworld.com:3690/CAPITAL958FMAAC.aac"},
  {"China Plus Radio","https://sk.cri.cn/am846.m3u8"},
  {"Class FM","http://22393.live.streamtheworld.com/CLASS95.mp3"},
  {"CNA938","http://playerservices.streamtheworld.com/api/livestream-redirect/938NOW_PREM.aac"},
  {"CRI 环球资讯广播 FM90.5","http://sk.cri.cn/905.m3u8"},
  {"CRI世界华声","http://sk.cri.cn/hxfh.m3u8"},
  {"安徽交通广播","https://satellitepull.cnr.cn/live/wxahjtgb/playlist.m3u8"},
  {"安徽经济广播","https://satellitepull.cnr.cn/live/wxahjjgb/playlist.m3u8"},
  {"安徽旅游广播","https://satellitepull.cnr.cn/live/wxahlygb/playlist.m3u8"},
  {"安徽农村广播","https://satellitepull.cnr.cn/live/wxahncgb/playlist.m3u8"},
  {"安徽生活广播","https://satellitepull.cnr.cn/live/wxahshgb/playlist.m3u8"},
  {"安徽戏曲广播","https://satellitepull.cnr.cn/live/wxahxqgb/playlist.m3u8"},
  {"安徽小说评书","https://satellitepull.cnr.cn/live/wxahxspsgb/playlist.m3u8"},
  {"安徽之声","https://satellitepull.cnr.cn/live/wxahxxgb/playlist.m3u8"},
  {"巴渝之声 FM104.5","http://ls.qingting.fm/live/3545693.m3u8"},
  {"保定交通广播 FM104.8","http://ls.qingting.fm/live/28140.m3u8"},
  {"保定经典964汽车音乐广播","http://ls.qingting.fm/live/2227017.m3u8"},
  {"保定新闻广播 FM93.7","http://ls.qingting.fm/live/3701149.m3u8"},
  {"保山综合广播 FM98.7","http://ls.qingting.fm/live/3702178.m3u8"},
  {"北京好音乐 FM95.9","http://ls.qingting.fm/live/2131011.m3u8"},
  {"北京文艺广播 FM87.6","http://live.xmcdn.com/live/94/64.m3u8"},
  {"兵团综合广播","https://satellitepull.cnr.cn/live/wxbtzs/playlist.m3u8"},
  {"常州交通广播 FM90","http://ls.qingting.fm/live/2796.m3u8"},
  {"郴州音乐交通广播 FM102.8","http://ls.qingting.fm/live/86747.m3u8"},
  {"郴州综合广播 FM99.2","http://ls.qingting.fm/live/76765.m3u8"},
  {"沈阳都市广播 FM92.1","http://ls.qingting.fm/live/1099.m3u8"},
  {"沈阳新闻广播 FM104.5","http://ls.qingting.fm/live/23891.m3u8"},
  {"沈阳音乐广播 路上好朋友 FM98.6","http://ls.qingting.fm/live/1101.m3u8"},
  {"楚天交通广播","https://satellitepull.cnr.cn/live/wx32hubctjtgb/playlist.m3u8"},
  {"第一财经广播","https://satellitepull.cnr.cn/live/wx32dycjgb/playlist.m3u8"},
  {"东广新闻台 FM90.9","http://ls.qingting.fm/live/275.m3u8"},
  {"福建财经961","https://satellitepull.cnr.cn/live/wx32fjdnjjgb/playlist.m3u8"},
  {"福建东南广播","https://satellitepull.cnr.cn/live/wx32fjdngb/playlist.m3u8"},
  {"福建都市广播","https://satellitepull.cnr.cn/live/wx32fjdndsgb/playlist.m3u8"},
  {"福建交通广播","https://satellitepull.cnr.cn/live/wx32fjdnjtgb/playlist.m3u8"},
  {"福建经济广播 FM96.1","http://live.xmcdn.com/live/789/64.m3u8"},
  {"福建私家车广播 FM98.7","http://live.xmcdn.com/live/793/64.m3u8"},
  {"福建新闻广播","https://satellitepull.cnr.cn/live/wx32fjxwgb/playlist.m3u8"},
  {"甘肃都市调频","https://satellitepull.cnr.cn/live/wxgsdstb/playlist.m3u8"},
  {"甘肃黄河之声","https://satellitepull.cnr.cn/live/wxgshhzs/playlist.m3u8"},
  {"甘肃交通广播","https://satellitepull.cnr.cn/live/wxgsjtgb/playlist.m3u8"},
  {"甘肃农村广播","https://satellitepull.cnr.cn/live/wxgsncgb/playlist.m3u8"},
  {"甘肃青春调频","https://satellitepull.cnr.cn/live/wxgsqcgb/playlist.m3u8"},
  {"甘肃新闻综合","https://satellitepull.cnr.cn/live/wxgsxwzhgb/playlist.m3u8"},
  {"广东城市之声","https://satellitepull.cnr.cn/live/wxgdcszs/playlist.m3u8"},
  {"广东股市广播","https://satellitepull.cnr.cn/live/wxgdgsgb/playlist.m3u8"},
  {"广东南方生活广播 FM93.6","http://live.xmcdn.com/live/249/64.m3u8"},
  {"广东南粤之声","https://satellitepull.cnr.cn/live/wxnyzs/playlist.m3u8"},
  {"广东文体广播","https://satellitepull.cnr.cn/live/wxgdwtgb/playlist.m3u8"},
  {"广东新闻频道 FM91.4","http://live.xmcdn.com/live/245/64.m3u8"},
  {"广东羊城交通广播 FM105.2","http://live.xmcdn.com/live/248/64.m3u8"},
  {"广东音乐之声 FM99.3","http://live.xmcdn.com/live/74/64.m3u8"},
  {"广东优悦广播(南粤) FM105.7","http://ls.qingting.fm/live/470.m3u8"},
  {"广东珠江经济台 FM97.4","http://live.xmcdn.com/live/252/64.m3u8"},
  {"广西北部湾之声(广西对外广播) FM96.4","http://ls.qingting.fm/live/1757.m3u8"},
  {"广西对外广播","https://satellitepull.cnr.cn/live/wx32gxdwgb/playlist.m3u8"},
  {"广西交通广播","https://satellitepull.cnr.cn/live/wx32gxjtgb/playlist.m3u8"},
  {"广西交通台 FM100.3","http://ls.qingting.fm/live/1758.m3u8"},
  {"广西教育生活","https://satellitepull.cnr.cn/live/wx32gbjyshgb/playlist.m3u8"},
  {"广西经济广播","https://satellitepull.cnr.cn/live/wx32gxjjgb/playlist.m3u8"},
  {"广西女主播电台 FM97.0","http://ls.qingting.fm/live/1754.m3u8"},
  {"广西人民广播","https://satellitepull.cnr.cn/live/wx32gxrmgb/playlist.m3u8"},
  {"广西私家车930 FM93.0","http://ls.qingting.fm/live/1756.m3u8"},
  {"广西文艺广播","https://satellitepull.cnr.cn/live/wx32gxwygb/playlist.m3u8"},
  {"广西新闻910 FM91.0","http://ls.qingting.fm/live/1753.m3u8"},
  {"广西音乐台 FM95.0","http://ls.qingting.fm/live/4875.m3u8"},
  {"广州 MYFM 88.0 (都市生活)","http://ls.qingting.fm/live/52712.m3u8"},
  {"广州花都广播 FM100.5","http://ls.qingting.fm/live/1263.m3u8"},
  {"广州交通电台 FM106.1","http://ls.qingting.fm/live/4955.m3u8"},
  {"广州汽车音乐电台 FM102.7","http://live.xmcdn.com/live/257/64.m3u8"},
  {"广州新闻电台 FM96.2","http://live.xmcdn.com/live/256/64.m3u8"},
  {"贵州电台交通广播 FM95.2","http://ls.qingting.fm/live/23927.m3u8"},
  {"贵州都市广播","https://satellitepull.cnr.cn/live/wx32gzqcgb/playlist.m3u8"},
  {"贵州故事广播","https://satellitepull.cnr.cn/live/wx32gzgsgb/playlist.m3u8"},
  {"贵州经济广播","https://satellitepull.cnr.cn/live/wx32gzjjgb/playlist.m3u8"},
  {"贵州旅游广播","https://satellitepull.cnr.cn/live/wx32gzlygb/playlist.m3u8"},
  {"贵州综合广播","https://satellitepull.cnr.cn/live/wx32gzwxwzhgb/playlist.m3u8"},
  {"海口音乐广播","http://ls.qingting.fm/live/23859.m3u8"},
  {"海南国际旅游之声 FM103.8","http://ls.qingting.fm/live/1862.m3u8"},
  {"海南交通广播","https://satellitepull.cnr.cn/live/wxhainjtgb/playlist.m3u8"},
  {"海南民生广播 FM101","http://ls.qingting.fm/live/1511803.m3u8"},
  {"海南新闻广播","https://satellitepull.cnr.cn/live/wxhainxwgb/playlist.m3u8"},
  {"河北故事广播 FM107.9","http://ls.qingting.fm/live/1645.m3u8"},
  {"河北交通广播 FM99.2","http://ls.qingting.fm/live/1646.m3u8"},
  {"河北交通广播","https://satellitepull.cnr.cn/live/wxhebjtgb/playlist.m3u8"},
  {"河北经济广播","https://satellitepull.cnr.cn/live/wxhebjjgb/playlist.m3u8"},
  {"河北旅游广播 AM603","http://ls.qingting.fm/live/1651.m3u8"},
  {"河北农民广播 AM558","http://ls.qingting.fm/live/1650.m3u8"},
  {"河北生活广播","https://satellitepull.cnr.cn/live/wxhebshgb/playlist.m3u8"},
  {"河北私家车广播 FM90.7","http://ls.qingting.fm/live/4868.m3u8"},
  {"河北新闻广播 FM104.3","http://ls.qingting.fm/live/1644.m3u8"},
  {"河北综合广播","https://satellitepull.cnr.cn/live/wxhebzhgb/playlist.m3u8"},
  {"河南My Radio广播","https://stream.hndt.com/live/yingshi/playlist.m3u8"},
  {"河南大象资讯台","https://stream.hndt.com/live/nongcun/playlist.m3u8"},
  {"河南交通广播","https://stream.hndt.com/live/jiaotong/playlist.m3u8"},
  {"河南教育广播","https://stream.hndt.com/live/jiaoyu/playlist.m3u8"},
  {"河南经济广播","https://satellitepull.cnr.cn/live/wxhnjjgb/playlist.m3u8"},
  {"河南旅游广播","https://satellitepull.cnr.cn/live/wxhnlygb/playlist.m3u8"},
  {"河南农村广播","https://satellitepull.cnr.cn/live/wxhnncgb/playlist.m3u8"},
  {"河南戏曲广播","https://satellitepull.cnr.cn/live/wxhnxqgb/playlist.m3u8"},
  {"河南新闻广播","https://satellitepull.cnr.cn/live/wxhnxwgb/playlist.m3u8"},
  {"河南信息广播","https://satellitepull.cnr.cn/live/wxhnxxgb/playlist.m3u8"},
  {"河南音乐广播","https://stream.hndt.com/live/yinyue/playlist.m3u8"},
  {"鹤壁交通音乐广播 FM93.5","http://ls.qingting.fm/live/3032681.m3u8"},
  {"鹤山电台104.7","http://ls.qingting.fm/live/1286.m3u8"},
  {"黑龙江爱家调频","https://satellitepull.cnr.cn/live/wx32hljajgb/playlist.m3u8"},
  {"黑龙江朝鲜语","https://satellitepull.cnr.cn/live/wx32hljcygb/playlist.m3u8"},
  {"黑龙江高校广播","https://satellitepull.cnr.cn/live/wx32hljgxgb/playlist.m3u8"},
  {"黑龙江交通广播","https://satellitepull.cnr.cn/live/wx32hljjtgb/playlist.m3u8"},
  {"黑龙江女性广播","https://satellitepull.cnr.cn/live/wx32hljnxgb/playlist.m3u8"},
  {"黑龙江私家车","https://satellitepull.cnr.cn/live/wx32hljsjcgb/playlist.m3u8"},
  {"黑龙江乡村广播","https://satellitepull.cnr.cn/live/wx32hljxcgb/playlist.m3u8"},
  {"黑龙江新闻广播","https://satellitepull.cnr.cn/live/wx32hljxwgb/playlist.m3u8"},
  {"黑龙江音乐广播","https://satellitepull.cnr.cn/live/wx32hljyygb/playlist.m3u8"},
  {"衡阳交通广播 FM101.8","http://ls.qingting.fm/live/5079921.m3u8"},
  {"衡阳新闻广播 FM98.9","http://ls.qingting.fm/live/5079970.m3u8"},
  {"呼和浩特交通广播 FM107.4","http://ls.qingting.fm/live/2218715.m3u8"},
  {"呼和浩特新闻综合广播 FM92.9","http://ls.qingting.fm/live/2218711.m3u8"},
  {"呼伦贝尔汉语广播","https://satellitepull.cnr.cn/live/wx32nmghlbehygb/playlist.m3u8"},
  {"呼伦贝尔蒙语广播","https://satellitepull.cnr.cn/live/wx32nmghlbemygb/playlist.m3u8"},
  {"湖北经典音乐","https://satellitepull.cnr.cn/live/wx32hubyygb/playlist.m3u8"},
  {"湖北经济广播","https://satellitepull.cnr.cn/live/wx32hubjjgb/playlist.m3u8"},
  {"湖北之声","https://satellitepull.cnr.cn/live/wx32hubzsgb/playlist.m3u8"},
  {"湖南新闻广播","https://satellitepull.cnr.cn/live/wx32hunxwgb/playlist.m3u8"},
  {"湖州交通文艺广播 FM98.5","http://ls.qingting.fm/live/2811.m3u8"},
  {"湖州经济广播 FM103.5","http://ls.qingting.fm/live/2812.m3u8"},
  {"湖州综合广播 湖州之声 FM05","http://ls.qingting.fm/live/2810.m3u8"},
  {"华语环球","https://sk.cri.cn/hyhq.m3u8"},
  {"惠州新闻综合广播 FM100","http://ls.qingting.fm/live/5016.m3u8"},
  {"惠州音乐广播 FM90.7","http://ls.qingting.fm/live/2212959.m3u8"},
  {"吉林交通广播","https://satellitepull.cnr.cn/live/wxjljtgb/playlist.m3u8"},
  {"吉林经济广播","https://satellitepull.cnr.cn/live/wxjljjgb/playlist.m3u8"},
  {"吉林乡村广播","https://satellitepull.cnr.cn/live/wxjlxcgb/playlist.m3u8"},
  {"吉林新闻综合","https://satellitepull.cnr.cn/live/wxjlxwzhgb/playlist.m3u8"},
  {"济南故事广播 FM104.3","http://ls.qingting.fm/live/1672.m3u8"},
  {"济南经济广播 FM90.9","http://ls.qingting.fm/live/1668.m3u8"},
  {"济南私家车广播 FM93.6","http://ls.qingting.fm/live/1670.m3u8"},
  {"江苏财经广播 AM585","http://lzlive.vojs.cn/caijing/92/live.m3u8"},
  {"江苏财经广播","https://satellitepull.cnr.cn/live/wx32jscjgb/playlist.m3u8"},
  {"江苏健康广播","https://satellitepull.cnr.cn/live/wx32jsjkgb/playlist.m3u8"},
  {"江苏交通广播","https://satellitepull.cnr.cn/live/wx32jsjtgb/playlist.m3u8"},
  {"江苏金陵之声","https://satellitepull.cnr.cn/live/wx32jsqctp/playlist.m3u8"},
  {"江苏经典流行音乐","https://satellitepull.cnr.cn/live/wx32jsjdlxyy/playlist.m3u8"},
  {"江苏新闻综合","https://satellitepull.cnr.cn/live/wx32jsxwzhgb/playlist.m3u8"},
  {"江西交通广播","https://satellitepull.cnr.cn/live/wx32jiangxjtgb/playlist.m3u8"},
  {"江西新闻广播","https://satellitepull.cnr.cn/live/wx32jiangxxwgb/playlist.m3u8"},
  {"九江交通广播 FM88.4 FM88.9","http://ls.qingting.fm/live/2785094.m3u8"},
  {"昆明汽车广播 FM95.4","http://ls.qingting.fm/live/1936.m3u8"},
  {"昆明阳光广播","http://ls.qingting.fm/live/1934.m3u8"},
  {"拉萨人民广播电台 FM91.4","http://ls.qingting.fm/live/3244137.m3u8"},
  {"辽宁交通广播","https://satellitepull.cnr.cn/live/wxlnjtgb/playlist.m3u8"},
  {"辽宁经济广播","https://satellitepull.cnr.cn/live/wxlnjjtb/playlist.m3u8"},
  {"辽宁文艺广播","https://satellitepull.cnr.cn/live/wxlnwygb/playlist.m3u8"},
  {"辽宁乡村广播","https://satellitepull.cnr.cn/live/wxlnxcgb/playlist.m3u8"},
  {"辽宁之声","https://satellitepull.cnr.cn/live/wxlnzhgb/playlist.m3u8"},
  {"龙广交通广播 FM99.8","http://ls.qingting.fm/live/4973.m3u8"},
  {"龙广青苹果之声 FM104.6","http://ls.qingting.fm/live/4976.m3u8"},
  {"梅州新闻广播 FM94.8","http://ls.qingting.fm/live/24173.m3u8"},
  {"每日歌曲","https://lhttp.qingting.fm/live/5021381/64k.mp3"},
  {"蒙古语综合广播","https://satellitepull.cnr.cn/live/wx32nmgmyxwgb/playlist.m3u8"},
  {"南方生活广播","https://satellitepull.cnr.cn/live/wxgdnfshgb/playlist.m3u8"},
  {"南宁交通音乐广播 FM107.4","http://ls.qingting.fm/live/80793.m3u8?aac"},
  {"南通交通广播 FM92.9","http://ls.qingting.fm/live/2216385.m3u8"},
  {"南通新闻广播 FM97.0","http://ls.qingting.fm/live/1611381.m3u8"},
  {"内蒙古对外广播","https://satellitepull.cnr.cn/live/wx32nmgdwgb/playlist.m3u8"},
  {"内蒙古汉语广播","https://satellitepull.cnr.cn/live/wx32nmghyzhxwgb/playlist.m3u8"},
  {"内蒙古交通之声","https://satellitepull.cnr.cn/live/wx32nmgjtgb/playlist.m3u8"},
  {"内蒙古绿野之声","https://satellitepull.cnr.cn/live/wx32nmglyzs/playlist.m3u8"},
  {"内蒙古蒙古语广播","https://satellitepull.cnr.cn/live/wx32nmgmygb/playlist.m3u8"},
  {"内蒙古音乐之声","https://satellitepull.cnr.cn/live/wx32nmgyygb/playlist.m3u8"},
  {"宁夏新闻广播","https://satellitepull.cnr.cn/live/wxnxxwgb/playlist.m3u8"},
  {"宁波动感105 FM105.2","http://ls.qingting.fm/live/3047946.m3u8"},
  {"宁波交通广播 FM93.9","http://ls.qingting.fm/live/1140.m3u8"},
  {"黔西南金州之声 FM107.9","http://ls.qingting.fm/live/5045.m3u8"},
  {"青岛故事广播 FM95.2","http://ls.qingting.fm/live/4956.m3u8"},
  {"青岛交通广播 FM89.7","http://ls.qingting.fm/live/1676.m3u8"},
  {"青岛西海岸城市生活广播 FM92.6","http://ls.qingting.fm/live/33446.m3u8"},
  {"青岛新闻广播 FM107.6","http://ls.qingting.fm/live/1673.m3u8"},
  {"青海藏语广播","https://satellitepull.cnr.cn/live/wx32qhzygb/playlist.m3u8"},
  {"青海交通音乐","https://satellitepull.cnr.cn/live/wx32qhjtyygb/playlist.m3u8"},
  {"青海交通音乐广播 FM97.2","http://ls.qingting.fm/live/5009.m3u8"},
  {"青海经济广播","https://satellitepull.cnr.cn/live/wx32qhjjgb/playlist.m3u8"},
  {"青海之声","https://satellitepull.cnr.cn/live/wx32qhwxzhgb/playlist.m3u8"},
  {"山东交通广播","https://satellitepull.cnr.cn/live/wxsdjtgb/playlist.m3u8"},
  {"山东经典音乐","https://audiolive302.iqilu.com/sdradioShenghuo/sdradio04/playlist.m3u8"},
  {"山东经济广播","https://satellitepull.cnr.cn/live/wxsdjjgb/playlist.m3u8"},
  {"山东女主播电台 FM97.5","http://ls.qingting.fm/live/60258.m3u8"},
  {"山东生活广播 MyFM FM105","http://ls.qingting.fm/live/60260.m3u8"},
  {"山东体育休闲","https://satellitepull.cnr.cn/live/wxsdtyxxgb/playlist.m3u8"},
  {"山东文艺广播","https://satellitepull.cnr.cn/live/wxsdwyssgb/playlist.m3u8"},
  {"山东乡村广播","https://satellitepull.cnr.cn/live/wxsdxcgb/playlist.m3u8"},
  {"山西综合广播","https://satellitepull.cnr.cn/live/wxssxxwgb/playlist.m3u8"},
  {"陕西交通广播","https://satellitepull.cnr.cn/live/wxsxxjtgb/playlist.m3u8"},
  {"陕西经济广播","https://satellitepull.cnr.cn/live/wxsxxjjgb/playlist.m3u8"},
  {"陕西农村广播","https://satellitepull.cnr.cn/live/wxsxxncgb/playlist.m3u8"},
  {"陕西青春广播","https://satellitepull.cnr.cn/live/wxsxxqcgb/playlist.m3u8"},
  {"陕西新闻广播","https://satellitepull.cnr.cn/live/wxsxxxwgb/playlist.m3u8"},
  {"上海东方广播","https://satellitepull.cnr.cn/live/wx32dfgbdt/playlist.m3u8"},
  {"深圳飞扬971","https://satellitepull.cnr.cn/live/wxszfy971/playlist.m3u8"},
  {"深圳交通频率","https://satellitepull.cnr.cn/live/wxszjjpl/playlist.m3u8"},
  {"深圳快乐1062(交通广播)","http://ls.qingting.fm/live/1272.m3u8"},
  {"深圳私家车","https://satellitepull.cnr.cn/live/wxszsjcgb/playlist.m3u8"},
  {"世界华声","https://sk.cri.cn/hxfh.m3u8"},
  {"四川交通广播","https://satellitepull.cnr.cn/live/wxscjtgb/playlist.m3u8"},
  {"四川民族广播 AM954","http://ls.qingting.fm/live/1115.m3u8"},
  {"四川民族频率","https://satellitepull.cnr.cn/live/wxscmzgb/playlist.m3u8"},
  {"太原交通广播 FM107","http://ls.qingting.fm/live/4900.m3u8"},
  {"太原私家车Radio FM104.4","http://ls.qingting.fm/live/4018.m3u8"},
  {"太原新闻广播 FM91.2","http://ls.qingting.fm/live/23873.m3u8"},
  {"太原音乐广播 FM102.6","http://ls.qingting.fm/live/1185.m3u8"},
  {"万盛旅游交通广播 FM92.2","http://ls.qingting.fm/live/5359760.m3u8"},
  {"潍坊私家车广播 FM93.3","http://ls.qingting.fm/live/84511.m3u8"},
  {"潍坊新闻广播 FM100.2","http://ls.qingting.fm/live/60358.m3u8"},
  {"潍坊音乐优生活 FM90.8","http://ls.qingting.fm/live/4865.m3u8"},
  {"温州交通广播 FM103.9","http://ls.qingting.fm/live/23863.m3u8"},
  {"温州经济生活广播 FM88.8","http://ls.qingting.fm/live/23867.m3u8"},
  {"温州绿色之声 FM93.8","http://ls.qingting.fm/live/1158.m3u8"},
  {"温州私家车音乐广播 FM100.3","http://ls.qingting.fm/live/23865.m3u8"},
  {"温州新闻广播 FM94.9","http://ls.qingting.fm/live/23861.m3u8"},
  {"无锡交通广播 FM106.9","http://ls.qingting.fm/live/2780.m3u8"},
  {"无锡新闻广播 FM93.7","http://ls.qingting.fm/live/2777.m3u8"},
  {"西安交通广播 FM104.3","http://ls.qingting.fm/live/1611.m3u8"},
  {"西安新闻广播 FM95.0","http://ls.qingting.fm/live/1610.m3u8"},
  {"西安音乐广播 FM93.1","http://ls.qingting.fm/live/1612.m3u8"},
  {"西藏藏语广播","https://satellitepull.cnr.cn/live/wxxzzygb/playlist.m3u8"},
  {"西藏藏语康巴方言","https://satellitepull.cnr.cn/live/wxxzzykbfy/playlist.m3u8"},
  {"西藏都市生活","https://satellitepull.cnr.cn/live/wxxzdsshgb/playlist.m3u8"},
  {"西藏对外交通","https://satellitepull.cnr.cn/live/wxxzdwjtgb/playlist.m3u8"},
  {"西藏汉语广播","https://satellitepull.cnr.cn/live/wxxzhygb/playlist.m3u8"},
  {"西宁交通频率","http://ls.qingting.fm/live/3400408.m3u8"},
  {"西宁新闻频率","http://ls.qingting.fm/live/3400403.m3u8"},
  {"襄阳交通广播 FM89.0","http://ls.qingting.fm/live/1307.m3u8"},
  {"襄阳音乐广播","http://ls.qingting.fm/live/5057.m3u8"},
  {"新疆哈语广播","https://satellitepull.cnr.cn/live/wxxjhygb/playlist.m3u8"},
  {"新疆交通广播","https://satellitepull.cnr.cn/live/wxxjjtgb/playlist.m3u8"},
  {"新疆柯尔克孜语广播","https://satellitepull.cnr.cn/live/wxxjkygb/playlist.m3u8"},
  {"新疆绿色广播","https://satellitepull.cnr.cn/live/wxxjlsgb/playlist.m3u8"},
  {"新疆蒙语广播","https://satellitepull.cnr.cn/live/wxxjmygb/playlist.m3u8"},
  {"新疆私家车广播 FM92.9","http://ls.qingting.fm/live/1909.m3u8"},
  {"新疆私家车广播","https://satellitepull.cnr.cn/live/wxxjsjcgb/playlist.m3u8"},
  {"新疆维吾尔语交通文艺广播","https://satellitepull.cnr.cn/live/wxxjwyjtwygb/playlist.m3u8"},
  {"新疆维语综合广播","https://satellitepull.cnr.cn/live/wxxjwyzhgb/playlist.m3u8"},
  {"新疆新闻广播","https://satellitepull.cnr.cn/live/wxxjxwgb/playlist.m3u8"},
  {"延边文艺广播","https://satellitepull.cnr.cn/live/wxybwyshgb/playlist.m3u8"},
  {"延边新闻广播","https://satellitepull.cnr.cn/live/wxybxwgb/playlist.m3u8"},
  {"羊城交通广播","https://satellitepull.cnr.cn/live/wxgdycjtt/playlist.m3u8"},
  {"阳泉交通广播","http://ls.qingting.fm/live/4592896.m3u8?aac"},
  {"阳泉新闻综合广播","http://ls.qingting.fm/live/5876899.m3u8?aac"},
  {"阳信人民广播电台 FM103.4","http://ls.qingting.fm/live/2915753.m3u8"},
  {"岳阳交通广播 FM106.1","http://ls.qingting.fm/live/88931.m3u8"},
  {"岳阳新闻综合广播","http://ls.qingting.fm/live/88933.m3u8"},
  {"云南国际广播","https://satellitepull.cnr.cn/live/wxynsegb/playlist.m3u8"},
  {"云南交通之声","https://satellitepull.cnr.cn/live/wxynjtgb/playlist.m3u8"},
  {"云南经济广播","https://satellitepull.cnr.cn/live/wxynjjgb/playlist.m3u8"},
  {"云南民族广播","https://satellitepull.cnr.cn/live/wxynmzgb/playlist.m3u8"},
  {"云南新闻广播","https://satellitepull.cnr.cn/live/wxynxwgb/playlist.m3u8"},
  {"长沙城市之声 FM101.7","http://ls.qingting.fm/live/4237.m3u8"},
  {"长沙新闻广播 FM105.0","http://ls.qingting.fm/live/4877.m3u8"},
  {"长治交通文艺广播 FM94.9","http://ls.qingting.fm/live/2669405.m3u8"},
  {"长治新闻综合广播(幸福广播) FM94.3","http://ls.qingting.fm/live/2702863.m3u8"},
  {"浙江财富广播 FM95","http://ls.qingting.fm/live/4519.m3u8"},
  {"浙江城市之声","https://satellitepull.cnr.cn/live/wxzjcszs/playlist.m3u8"},
  {"浙江动听(音乐调频) FM96.8","http://ls.qingting.fm/live/4866.m3u8"},
  {"浙江交通之声","https://satellitepull.cnr.cn/live/wxzjjtgb/playlist.m3u8"},
  {"浙江经济广播","https://satellitepull.cnr.cn/live/wxzjjjgb/playlist.m3u8"},
  {"浙江民生996","https://satellitepull.cnr.cn/live/wxzjmsgb/playlist.m3u8"},
  {"浙江女主播电台","https://satellitepull.cnr.cn/live/wxzj1045/playlist.m3u8"},
  {"浙江悦动之音","https://satellitepull.cnr.cn/live/wxzj968/playlist.m3u8"},
  {"郑州车道931","http://ls.qingting.fm/live/1221.m3u8"},
  {"郑州活力944","http://ls.qingting.fm/live/4921.m3u8"},
  {"郑州经典广播 FM107.9","http://ls.qingting.fm/live/1223.m3u8"},
  {"郑州汽车广播 FM91.2","http://ls.qingting.fm/live/1211.m3u8"},
  {"郑州私家车 FM91.8","http://ls.qingting.fm/live/1222.m3u8"},
  {"郑州新闻广播 FM98.6","http://ls.qingting.fm/live/1220.m3u8"},
  {"重庆都市广播 FM93.8","http://live.xmcdn.com/live/132/64.m3u8"},
  {"重庆都市广播","https://satellitepull.cnr.cn/live/wxcqdsgb/playlist.m3u8"},
  {"重庆经济广播","https://satellitepull.cnr.cn/live/wxcqjjgb/playlist.m3u8"},
  {"重庆新闻广播","https://satellitepull.cnr.cn/live/wxcqxwgb/playlist.m3u8"},
  {"珠海电台交通音乐875","http://ls.qingting.fm/live/1275.m3u8"},
  {"珠海电台先锋951","http://ls.qingting.fm/live/1274.m3u8"},
  {"珠江经济台","https://satellitepull.cnr.cn/live/wxgdzjjjt/playlist.m3u8"},
};
constexpr size_t kBuiltinStationCount = sizeof(kBuiltinStations) / sizeof(kBuiltinStations[0]);
// END INLINED: built-in station catalog


// BEGIN INLINED: station icon catalog
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
  {"北京新闻广播","https://satellitepull.cnr.cn/live/wxbjxwgb/playlist.m3u8","5b871d6c0e342ac97800183b.jpg"},
  {"北京文艺广播","http://ls.qingting.fm/live/333.m3u8","8b0448bb6e937dfdcd75e307.jpg"},
  {"北京城市广播","https://satellitepull.cnr.cn/live/wxbjcsfwgl/playlist.m3u8","3a95a52f9982e6340cb45ad2.jpg"},
  {"北京体育广播","https://brtv-radiolive.rbc.cn/alive/fm1025.m3u8","5954f4582d37d853c887a67d.jpg"},
  {"北京阳光调频","https://lhttp.qtfm.cn/live/5021739/64k.mp3","c65819c67d152630c6f1e2be.jpg"},
  {"北京经典调频","https://radio.0472.org/?id=1254","bef49f59ac1c14dcccf9e510.jpg"},
  {"CRI华语环球","http://sk.cri.cn/hyhq.m3u8","fa2b891e16339d1a08ca763f.jpg"},
  {"CRI环球资讯","https://sk.cri.cn/905.m3u8","3460bbdf6e5c11588c0acc34.jpg"},
  {"CRI南海之声","https://sk.cri.cn/nhzs.m3u8","942803712e32a906aa54b85c.jpg"},
  {"CRI英语资讯","http://sk.cri.cn/am846.m3u8","2c363ecbf3566c5847a25fb3.jpg"},
  {"RTHK3","https://rthkradio3-live.akamaized.net/hls/live/2040079/radio3/master.m3u8","dc2e2b29d8b1a6919fff1e0d.jpg"},
  {"香港电台普通话台","https://rthkradiopth-live.akamaized.net/hls/live/2040082/radiopth/master.m3u8","3d55d6e3beef84e299c59a2d.jpg"},
  {"湖南经济广播","https://satellitepull.cnr.cn/live/wx32hunjjgb/playlist.m3u8","3d0266b80490d0efb967f8e6.jpg"},
  {"湖南新闻频道","https://radio.0472.org/?id=525","2f71407ae570bd62b36d0102.jpg"},
  {"湖南潇湘之声","https://satellitepull.cnr.cn/live/wx32hunyygb/playlist.m3u8","808915ea385b3175746417a3.jpg"},
  {"重庆文艺广播","https://satellitepull.cnr.cn/live/wxcqwygb/playlist.m3u8","48780aaa2006e266b3c10cfa.jpg"},
  {"上海故事广播","http://live.cooltv.top/tv/news1296.php?id=10","17bc1b0ceb1212a3df5f2069.jpg"},
  {"山西文艺广播","http://radiolive.sxrtv.com/live/wenyi/playlist.m3u8","fac333181a112b3469434019.jpg"},
  {"江苏文艺广播","https://satellitepull.cnr.cn/live/wx32jswygb/playlist.m3u8","b9a7fda7bc703cdb3a06c2ac.jpg"},
  {"江苏故事广播","https://satellitepull.cnr.cn/live/wx32jsgsgb/playlist.m3u8","d926dca78e0bba1605ce320f.jpg"},
  {"上海戏曲广播","https://radio.0472.org/?id=1314","e02dd076a74642fc945ecbce.jpg"},
  {"陕西故事广播","https://radio.0472.org/?id=1133","fb8c996d985e787d1d2c8fac.jpg"},
  {"北京音乐广播","http://ls.qingting.fm/live/332.m3u8","bc660c9cba38392c7d5bbfc3.jpg"},
  {"湖南金鹰之声","https://satellitepull.cnr.cn/live/wx32955/playlist.m3u8","980ba8b80eba70a2b8438849.jpg"},
  {"芒果时空音乐","https://radio.0472.org/?id=524","1050aa2ab4b2adf60493889a.jpg"},
  {"湖南交通广播","https://satellitepull.cnr.cn/live/wx32hunjtgb/playlist.m3u8","0ef6098643e56f5cedcc3e69.jpg"},
  {"湖南音乐之声","https://radio.0472.org/?id=1060","39c953da121e08cbd334688a.jpg"},
  {"长沙交通广播","https://radio.0472.org/?id=1061","9cdbef7ee5b4a26ff240f668.jpg"},
  {"长沙音乐广播","https://radio.0472.org/?id=1531","bc42f66ac5130cad2d006d4a.jpg"},
  {"广东音乐之声","https://satellitepull.cnr.cn/live/wxgdyyzs/playlist.m3u8","872c85048e00e9edf84679e7.jpg"},
  {"江西音乐广播","https://satellitepull.cnr.cn/live/wx32jiangxyygb/playlist.m3u8","2ee3a75bca5f658140643f87.jpg"},
  {"河北音乐广播","https://satellitepull.cnr.cn/live/wxhebyygb/playlist.m3u8","f2414fa4ae8f72f7038c8f25.jpg"},
  {"深圳音乐频率","https://radio.0472.org/?id=498","5f81dd29eeefd78aafaa7940.jpg"},
  {"南京音乐广播","http://hls.njgb.com/live_hls/4/playlist.m3u8","9294af1b36817696e476acb2.jpg"},
  {"江苏音乐广播","https://satellitepull.cnr.cn/live/wx32jsyygb/playlist.m3u8","56739bdfc3c8817b84a804a4.jpg"},
  {"河北汽车音乐","https://radio.pull.hebtv.com/live/hebqcyy.m3u8","e28da11ec2608ad62f2fd308.jpg"},
  {"重庆音乐广播","https://satellitepull.cnr.cn/live/wxcqyygb/playlist.m3u8","6bc81b5586d6a0572248b112.jpg"},
  {"龙江音乐广播","https://radio.0472.org/?id=552","50f09b8c2dcdfeb55bfe09e5.jpg"},
  {"内蒙音乐之声","https://radio.0472.org/?id=572","60c3e570e8b2340d0beff10f.jpg"},
  {"宁夏音乐广播","https://satellitepull.cnr.cn/live/wxnxyygb/playlist.m3u8","bb5c62227958fa26cc071e3e.jpg"},
  {"陕西音乐广播","https://satellitepull.cnr.cn/live/wxsxxyygb/playlist.m3u8","74b59ccf0f982f3b1710aa09.jpg"},
  {"青海音乐广播","https://radio.0472.org/?id=616","8ff1c18ce7b14b75ee39bbd4.jpg"},
  {"山西音乐广播","https://radio.0472.org/?id=637","492e8d9029cccef597d0add3.jpg"},
  {"山东音乐广播","https://satellitepull.cnr.cn/live/wxsdyygb/playlist.m3u8","6ea194b3b461505fc6406f1c.jpg"},
  {"安徽音乐广播","https://satellitepull.cnr.cn/live/wxahyygb/playlist.m3u8","abf2e98edc9feb2d985476fb.jpg"},
  {"江苏经典流行","http://satellitepull.cnr.cn/live/wx32jsjdlxyy/playlist.m3u8","38fc875f711411b135042fa9.jpg"},
  {"浙江音乐调频","https://radio.0472.org/?id=928","d7737794daf2ebc945c94e31.jpg"},
  {"厦门音乐广播","https://radio.0472.org/?id=1897","cb94d50dad714abbab21922e.jpg"},
  {"云南音乐广播","https://satellitepull.cnr.cn/live/wxynyygb/playlist.m3u8","d7722296ba934732eec00c2b.jpg"},
  {"广西音乐台","https://radio.0472.org/?id=590","e2561d1fb54fd9b3ce2a345b.jpg"},
  {"贵州音乐广播","https://satellitepull.cnr.cn/live/wx32gzyygb/playlist.m3u8","0b0497b093c9485f47a572bf.jpg"},
  {"新疆音乐广播","https://radio.0472.org/?id=1158","4166e1b2d9af8601c24f7e9e.jpg"},
  {"海南音乐广播","https://satellitepull.cnr.cn/live/wxhainyygb/playlist.m3u8","dc8cd36bb89b027e71bd4ff0.jpg"},
  {"河北文艺广播","https://satellitepull.cnr.cn/live/wxhebwygb/playlist.m3u8","c6fec9976f72dac4563b488b.jpg"},
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
  {"China Plus Radio","https://sk.cri.cn/am846.m3u8","0dca1764c310c21abdd67767.png"},
  {"安徽交通广播","https://satellitepull.cnr.cn/live/wxahjtgb/playlist.m3u8","7e296ba226326817213b385c.png"},
  {"安徽经济广播","https://satellitepull.cnr.cn/live/wxahjjgb/playlist.m3u8","ffa4c225c1e6941680cd4760.png"},
  {"安徽戏曲广播","https://satellitepull.cnr.cn/live/wxahxqgb/playlist.m3u8","c0c3154f78441562d88dae48.png"},
  {"安徽之声","https://satellitepull.cnr.cn/live/wxahxxgb/playlist.m3u8","f2fdfc5625d9fdaaab095ccd.png"},
  {"楚天交通广播","https://satellitepull.cnr.cn/live/wx32hubctjtgb/playlist.m3u8","d57d777eaac99c751f986b9c.png"},
  {"第一财经广播","https://satellitepull.cnr.cn/live/wx32dycjgb/playlist.m3u8","e2bf8b498c6db2f70257a855.png"},
  {"福建东南广播","https://satellitepull.cnr.cn/live/wx32fjdngb/playlist.m3u8","eb997e58a8428cb1fcbc8e7b.png"},
  {"福建都市广播","https://satellitepull.cnr.cn/live/wx32fjdndsgb/playlist.m3u8","c16eea6f48d1502faff7e6bf.png"},
  {"福建交通广播","https://satellitepull.cnr.cn/live/wx32fjdnjtgb/playlist.m3u8","f1973e2be0b0e987e44f56e3.png"},
  {"福建新闻广播","https://satellitepull.cnr.cn/live/wx32fjxwgb/playlist.m3u8","682beda6790c58d07e5a27c5.png"},
  {"甘肃都市调频","https://satellitepull.cnr.cn/live/wxgsdstb/playlist.m3u8","5a7a880ad4294c252c211c58.png"},
  {"甘肃交通广播","https://satellitepull.cnr.cn/live/wxgsjtgb/playlist.m3u8","889ba573ef07b89926e3f6c7.png"},
  {"甘肃农村广播","https://satellitepull.cnr.cn/live/wxgsncgb/playlist.m3u8","204ce336aa622f144721b0c4.png"},
  {"甘肃青春调频","https://satellitepull.cnr.cn/live/wxgsqcgb/playlist.m3u8","8ca4d185f4d8f13773c26701.png"},
  {"广东城市之声","https://satellitepull.cnr.cn/live/wxgdcszs/playlist.m3u8","7b9cd86297d6d4ee012d90e5.png"},
  {"广东股市广播","https://satellitepull.cnr.cn/live/wxgdgsgb/playlist.m3u8","3d550087ca21c6b565f1c2b7.png"},
  {"广东南粤之声","https://satellitepull.cnr.cn/live/wxnyzs/playlist.m3u8","660320b45fe6de592e3f23ed.png"},
  {"广东文体广播","https://satellitepull.cnr.cn/live/wxgdwtgb/playlist.m3u8","84f70d394e5ec0413385b9f8.png"},
  {"广西交通广播","https://satellitepull.cnr.cn/live/wx32gxjtgb/playlist.m3u8","e4abfad84d92287d0f52a1fd.png"},
  {"贵州都市广播","https://satellitepull.cnr.cn/live/wx32gzqcgb/playlist.m3u8","8822521b7eaddb0a52ff2ce2.png"},
  {"贵州故事广播","https://satellitepull.cnr.cn/live/wx32gzgsgb/playlist.m3u8","ff4d8eba10f67efa2a881aef.png"},
  {"贵州经济广播","https://satellitepull.cnr.cn/live/wx32gzjjgb/playlist.m3u8","9d0a338fc2e3a236af598d22.png"},
  {"贵州旅游广播","https://satellitepull.cnr.cn/live/wx32gzlygb/playlist.m3u8","835ac28044a3151e3a3ad587.png"},
  {"贵州综合广播","https://satellitepull.cnr.cn/live/wx32gzwxwzhgb/playlist.m3u8","c727384837e981827455711a.png"},
  {"海口音乐广播","http://ls.qingting.fm/live/23859.m3u8","de88cd51e59aa40e537ed80d.png"},
  {"海南交通广播","https://satellitepull.cnr.cn/live/wxhainjtgb/playlist.m3u8","23ef65cdc74518fd5b1a83c9.png"},
  {"海南新闻广播","https://satellitepull.cnr.cn/live/wxhainxwgb/playlist.m3u8","abb4d3ca1668777527a54990.png"},
  {"河北交通广播","https://satellitepull.cnr.cn/live/wxhebjtgb/playlist.m3u8","39af8ddfa2a9a40cfe6afa7e.png"},
  {"河北生活广播","https://satellitepull.cnr.cn/live/wxhebshgb/playlist.m3u8","e10c400239a312392fb7667e.png"},
  {"河北综合广播","https://satellitepull.cnr.cn/live/wxhebzhgb/playlist.m3u8","e7ecb9bbddb77f202df85369.png"},
  {"河南交通广播","https://stream.hndt.com/live/jiaotong/playlist.m3u8","30854eac2f4f2b2eb70d3e7a.png"},
  {"河南教育广播","https://stream.hndt.com/live/jiaoyu/playlist.m3u8","e95bd5fe9ea1ddf3532108d1.jpg"},
  {"河南经济广播","https://satellitepull.cnr.cn/live/wxhnjjgb/playlist.m3u8","cf9dfedc98761992c63471f3.png"},
  {"河南戏曲广播","https://satellitepull.cnr.cn/live/wxhnxqgb/playlist.m3u8","46e8949e4647b24039c28f1d.png"},
  {"河南新闻广播","https://satellitepull.cnr.cn/live/wxhnxwgb/playlist.m3u8","a116bd46538651c417aff86b.png"},
  {"河南音乐广播","https://stream.hndt.com/live/yinyue/playlist.m3u8","091d05d2505dfc43704a75cf.png"},
  {"黑龙江高校广播","https://satellitepull.cnr.cn/live/wx32hljgxgb/playlist.m3u8","d2a26f533e0e3a5a0fc997f0.png"},
  {"黑龙江乡村广播","https://satellitepull.cnr.cn/live/wx32hljxcgb/playlist.m3u8","51cdd434c15f9131e07203b8.png"},
  {"黑龙江新闻广播","https://satellitepull.cnr.cn/live/wx32hljxwgb/playlist.m3u8","b62ef9454727ceca135c0de3.png"},
  {"黑龙江音乐广播","https://satellitepull.cnr.cn/live/wx32hljyygb/playlist.m3u8","43b705783bc6c45099dab50e.png"},
  {"湖北之声","https://satellitepull.cnr.cn/live/wx32hubzsgb/playlist.m3u8","112850f07d7d051be319f6a3.png"},
  {"吉林交通广播","https://satellitepull.cnr.cn/live/wxjljtgb/playlist.m3u8","79e9d1edef48071c54a138b2.png"},
  {"吉林经济广播","https://satellitepull.cnr.cn/live/wxjljjgb/playlist.m3u8","abc4df0fcc753000d02f651f.png"},
  {"吉林乡村广播","https://satellitepull.cnr.cn/live/wxjlxcgb/playlist.m3u8","1875a2601b83ff5b5ed7c500.png"},
  {"江苏财经广播","https://satellitepull.cnr.cn/live/wx32jscjgb/playlist.m3u8","3bde4554a1068a3d45dcf938.png"},
  {"江苏健康广播","https://satellitepull.cnr.cn/live/wx32jsjkgb/playlist.m3u8","2f1b58b3b96b65b645ef4731.png"},
  {"江苏交通广播","https://satellitepull.cnr.cn/live/wx32jsjtgb/playlist.m3u8","01d4fb1a44762d800d9bac9b.png"},
  {"江西交通广播","https://satellitepull.cnr.cn/live/wx32jiangxjtgb/playlist.m3u8","7dbe587338004fe65635cb5f.png"},
  {"江西新闻广播","https://satellitepull.cnr.cn/live/wx32jiangxxwgb/playlist.m3u8","63c9095f58a0c2ea74dc8f41.png"},
  {"辽宁交通广播","https://satellitepull.cnr.cn/live/wxlnjtgb/playlist.m3u8","782da3822a61d7dc075cc785.png"},
  {"辽宁经济广播","https://satellitepull.cnr.cn/live/wxlnjjtb/playlist.m3u8","711021bee1f8ba35ec86d1d2.png"},
  {"辽宁乡村广播","https://satellitepull.cnr.cn/live/wxlnxcgb/playlist.m3u8","8e8738440bcac2f31403b8c0.png"},
  {"辽宁之声","https://satellitepull.cnr.cn/live/wxlnzhgb/playlist.m3u8","be503825cfcedb7692f9d7c8.png"},
  {"南方生活广播","https://satellitepull.cnr.cn/live/wxgdnfshgb/playlist.m3u8","1de2b0ac38fe768857911599.png"},
  {"内蒙古音乐之声","https://satellitepull.cnr.cn/live/wx32nmgyygb/playlist.m3u8","957223a821c3d60a46dce837.png"},
  {"宁夏新闻广播","https://satellitepull.cnr.cn/live/wxnxxwgb/playlist.m3u8","b7a734c34bd5156dc3a4272f.png"},
  {"青海经济广播","https://satellitepull.cnr.cn/live/wx32qhjjgb/playlist.m3u8","6009a93a8765a5d847b9de9b.png"},
  {"山东交通广播","https://satellitepull.cnr.cn/live/wxsdjtgb/playlist.m3u8","7634e41027feefedf3646d07.png"},
  {"山东经济广播","https://satellitepull.cnr.cn/live/wxsdjjgb/playlist.m3u8","334048a540ba17a5bc589501.png"},
  {"山东文艺广播","https://satellitepull.cnr.cn/live/wxsdwyssgb/playlist.m3u8","4250e48ac01afdab162ad3fd.png"},
  {"山东乡村广播","https://satellitepull.cnr.cn/live/wxsdxcgb/playlist.m3u8","974ae4d229bf14de38f5ee3e.png"},
  {"山西综合广播","https://satellitepull.cnr.cn/live/wxssxxwgb/playlist.m3u8","f0cf89b9851886c38d85f31f.png"},
  {"陕西交通广播","https://satellitepull.cnr.cn/live/wxsxxjtgb/playlist.m3u8","732057e3c9e671850e13650c.png"},
  {"陕西经济广播","https://satellitepull.cnr.cn/live/wxsxxjjgb/playlist.m3u8","99f586443259bea5fc1f3426.png"},
  {"陕西农村广播","https://satellitepull.cnr.cn/live/wxsxxncgb/playlist.m3u8","572748b00ee884f54d677b99.png"},
  {"陕西新闻广播","https://satellitepull.cnr.cn/live/wxsxxxwgb/playlist.m3u8","457884fceb78745ad34ca424.png"},
  {"深圳飞扬971","https://satellitepull.cnr.cn/live/wxszfy971/playlist.m3u8","8981fc88774f27a99b7306f2.png"},
  {"世界华声","https://sk.cri.cn/hxfh.m3u8","833ff3eccd3144c7f90a9876.png"},
  {"四川交通广播","https://satellitepull.cnr.cn/live/wxscjtgb/playlist.m3u8","1bac8364cf9bee6c8d2ae3a1.png"},
  {"延边新闻广播","https://satellitepull.cnr.cn/live/wxybxwgb/playlist.m3u8","7bcf5fad9254b1cf8e84b8bf.png"},
  {"羊城交通广播","https://satellitepull.cnr.cn/live/wxgdycjtt/playlist.m3u8","0865b0f1445e452f18d72e0f.png"},
  {"云南国际广播","https://satellitepull.cnr.cn/live/wxynsegb/playlist.m3u8","62953dbba00200d1aa80390d.png"},
  {"云南交通之声","https://satellitepull.cnr.cn/live/wxynjtgb/playlist.m3u8","ed0de72a1874d56ec9619b52.png"},
  {"云南经济广播","https://satellitepull.cnr.cn/live/wxynjjgb/playlist.m3u8","e3e60a7c6650c5a7fd918de0.png"},
  {"云南民族广播","https://satellitepull.cnr.cn/live/wxynmzgb/playlist.m3u8","1168b56af82fbc5c4d6ea446.png"},
  {"云南新闻广播","https://satellitepull.cnr.cn/live/wxynxwgb/playlist.m3u8","19fb2c4460a040aa9da10bb9.png"},
  {"浙江城市之声","https://satellitepull.cnr.cn/live/wxzjcszs/playlist.m3u8","e8ff759d5c8057467e461cd2.png"},
  {"浙江交通之声","https://satellitepull.cnr.cn/live/wxzjjtgb/playlist.m3u8","dd0827c15b3a1fd8792fd3bc.png"},
  {"浙江经济广播","https://satellitepull.cnr.cn/live/wxzjjjgb/playlist.m3u8","f5c4200b8137e2ba5e13bf1d.png"},
  {"重庆都市广播","https://satellitepull.cnr.cn/live/wxcqdsgb/playlist.m3u8","4c899cec8870e4712ba8ae4f.png"},
  {"重庆经济广播","https://satellitepull.cnr.cn/live/wxcqjjgb/playlist.m3u8","046caf2e0906211424c28070.png"},
};
constexpr size_t kIconStationCount=sizeof(kIconStations)/sizeof(kIconStations[0]);
// END INLINED: station icon catalog

#include <LittleFS.h>

#include <Update.h>

namespace {

constexpr char kSecurityNamespace[] = "security";
constexpr char kAdminPasswordKey[] = "admin_pass";
constexpr char kUiNamespace[] = "ui";
constexpr char kLedNamespace[] = "led";
constexpr char kAdminUser[] = "admin";
constexpr uint8_t kLogCapacity = 36;
constexpr uint32_t kWifiRetryMs = 10000;
constexpr uint32_t kPlaybackRetryInitialMs = 8000;
constexpr uint32_t kPlaybackRetryMaxMs = 60000;
constexpr uint32_t kPlaybackStartupTimeoutMs = 30000;
constexpr uint32_t kPlaybackPostReadyRetryMs = 1000;
constexpr char kBootChimePath[] = "/boot-chime.wav";
constexpr uint32_t kBootChimeMaxMs = 5000;
constexpr uint8_t kBootChimeVolume = 21;
constexpr uint8_t kStatusLedPin = 48;
constexpr uint8_t kStatusLedBrightness = 36;
constexpr uint8_t kPlayingLedBrightness = 255;
constexpr uint32_t kPlayingLedRainbowPeriodMs = 8000;
constexpr uint32_t kStatusLedRefreshMs = 20;
constexpr size_t kLegacyBuiltinStationCount = 100;
// Bump the import markers after the resource image migration so devices whose
// LittleFS playlist was replaced rebuild every missing bundled station once.
constexpr char kLegacyBuiltinCatalogKey[] = "builtin_100_v2";
constexpr char kExpandedStationPackKey[] = "station_pack_v4";
constexpr char kRegionSortKey[] = "region_sort_v5";
constexpr char kGroupOrderKey[] = "group_order_v1";
constexpr char kGroupLayoutKey[] = "group_layout_v1";
static_assert(kBuiltinStationCount == 397,
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
enum class PlayingLedEffect : uint8_t {
  Off,
  Rainbow,
  ColorBreathe,
  Aurora,
  Flame,
  Heartbeat,
  Meteor,
  Pulse,
  RandomFade,
  Music,
  Signal,
  FixedBreathe,
  Temperature,
  Starlight
};

struct RgbColor {
  uint8_t red;
  uint8_t green;
  uint8_t blue;
};

struct PlayingLedEffectOption {
  const char *id;
  PlayingLedEffect effect;
};

constexpr PlayingLedEffectOption kPlayingLedEffectOptions[] = {
    {"off", PlayingLedEffect::Off},
    {"rainbow", PlayingLedEffect::Rainbow},
    {"color_breathe", PlayingLedEffect::ColorBreathe},
    {"aurora", PlayingLedEffect::Aurora},
    {"flame", PlayingLedEffect::Flame},
    {"heartbeat", PlayingLedEffect::Heartbeat},
    {"meteor", PlayingLedEffect::Meteor},
    {"pulse", PlayingLedEffect::Pulse},
    {"random_fade", PlayingLedEffect::RandomFade},
    {"music", PlayingLedEffect::Music},
    {"signal", PlayingLedEffect::Signal},
    {"fixed_breathe", PlayingLedEffect::FixedBreathe},
    {"temperature", PlayingLedEffect::Temperature},
    {"starlight", PlayingLedEffect::Starlight},
};

PlayingLedEffect playingLedEffect = PlayingLedEffect::Rainbow;
uint32_t playingLedFixedColor = 0x0080FF;

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

RgbColor scaleRgb(RgbColor color, uint8_t level) {
  color.red = (static_cast<uint16_t>(color.red) * level + 127U) / 255U;
  color.green = (static_cast<uint16_t>(color.green) * level + 127U) / 255U;
  color.blue = (static_cast<uint16_t>(color.blue) * level + 127U) / 255U;
  return color;
}

RgbColor blendRgb(RgbColor from, RgbColor to, uint8_t amount) {
  return {
      static_cast<uint8_t>(from.red +
                           (static_cast<int16_t>(to.red) - from.red) *
                               amount / 255),
      static_cast<uint8_t>(from.green +
                           (static_cast<int16_t>(to.green) - from.green) *
                               amount / 255),
      static_cast<uint8_t>(from.blue +
                           (static_cast<int16_t>(to.blue) - from.blue) *
                               amount / 255),
  };
}

RgbColor rainbowRgb(uint16_t wheel) {
  wheel %= 768U;
  const uint8_t offset = wheel & 0xFFU;
  switch (wheel >> 8) {
    case 0: return {static_cast<uint8_t>(255U - offset), offset, 0};
    case 1: return {0, static_cast<uint8_t>(255U - offset), offset};
    default: return {offset, 0, static_cast<uint8_t>(255U - offset)};
  }
}

uint8_t breathingLevel(uint32_t now, uint32_t periodMs,
                       uint8_t minimum = 8) {
  const uint32_t halfPeriodMs = periodMs / 2U;
  const uint32_t position = now % periodMs;
  const uint16_t ramp = position < halfPeriodMs
                            ? position * 255U / halfPeriodMs
                            : (periodMs - position) * 255U / halfPeriodMs;
  return minimum + static_cast<uint32_t>(ramp) * ramp *
                       (255U - minimum) / 65025U;
}

uint8_t trianglePulse(uint32_t position, uint32_t center,
                      uint32_t halfWidth) {
  const uint32_t distance = position > center ? position - center
                                               : center - position;
  if (distance >= halfWidth) return 0;
  return (halfWidth - distance) * 255U / halfWidth;
}

uint32_t ledHash(uint32_t value) {
  value ^= value >> 16;
  value *= 0x7FEB352DU;
  value ^= value >> 15;
  value *= 0x846CA68BU;
  return value ^ (value >> 16);
}

RgbColor renderPlayingLedEffect(uint32_t now) {
  switch (playingLedEffect) {
    case PlayingLedEffect::Off:
      return {0, 0, 0};
    case PlayingLedEffect::ColorBreathe: {
      const RgbColor color = rainbowRgb((now % 12000U) * 768U / 12000U);
      return scaleRgb(color, breathingLevel(now, 4000U));
    }
    case PlayingLedEffect::Aurora: {
      constexpr RgbColor colors[] = {
          {0, 220, 180}, {0, 70, 255}, {130, 0, 255},
          {0, 255, 100}, {0, 220, 180}};
      const uint32_t position = now % 12000U;
      const uint8_t section = position / 3000U;
      const uint8_t amount = (position % 3000U) * 255U / 3000U;
      return scaleRgb(blendRgb(colors[section], colors[section + 1], amount),
                      210);
    }
    case PlayingLedEffect::Flame: {
      const uint32_t noise = ledHash(now / 70U);
      return {static_cast<uint8_t>(140U + noise % 116U),
              static_cast<uint8_t>(18U + (noise >> 8) % 92U),
              static_cast<uint8_t>((noise >> 20) % 9U)};
    }
    case PlayingLedEffect::Heartbeat: {
      const uint32_t position = now % 1600U;
      const uint8_t first = trianglePulse(position, 110U, 110U);
      const uint8_t second = trianglePulse(position, 390U, 140U);
      const uint8_t level = first > second ? first : second;
      return scaleRgb({255, 0, 36}, level);
    }
    case PlayingLedEffect::Meteor: {
      const uint32_t position = now % 1800U;
      const uint8_t level = position < 100U
                                ? position * 255U / 100U
                                : (1800U - position) * 255U / 1700U;
      return scaleRgb({170, 220, 255}, level);
    }
    case PlayingLedEffect::Pulse: {
      const uint32_t position = now % 1700U;
      const uint8_t level = position < 1400U
                                ? 8U + position * 247U / 1400U
                                : 0;
      return scaleRgb({170, 35, 255}, level);
    }
    case PlayingLedEffect::RandomFade: {
      const uint32_t interval = now / 3500U;
      const uint16_t fromWheel = ledHash(interval) % 768U;
      const uint16_t toWheel = ledHash(interval + 1U) % 768U;
      const uint32_t linear = (now % 3500U) * 255U / 3500U;
      const uint8_t smooth = linear * linear * (765U - 2U * linear) /
                             65025U;
      return blendRgb(rainbowRgb(fromWheel), rainbowRgb(toWheel), smooth);
    }
    case PlayingLedEffect::Music: {
      static uint32_t envelope = 0;
      static uint32_t reference = 1;
      const uint32_t peak = audioOutputPeak;
      audioOutputPeak = 0;
      envelope = peak > envelope
                     ? peak
                     : static_cast<uint64_t>(envelope) * 94U / 100U;
      reference = envelope > reference ? envelope
                                       : reference - reference / 1000U;
      if (reference == 0) reference = 1;
      uint32_t response = static_cast<uint64_t>(envelope) * 239U /
                          reference;
      if (response > 239U) response = 239U;
      const uint8_t level = 16U + response;
      return scaleRgb(rainbowRgb((now % 10000U) * 768U / 10000U), level);
    }
    case PlayingLedEffect::Signal: {
      int32_t quality = (WiFi.RSSI() + 90) * 255 / 50;
      if (quality < 0) quality = 0;
      if (quality > 255) quality = 255;
      return {static_cast<uint8_t>(255 - quality),
              static_cast<uint8_t>(quality),
              static_cast<uint8_t>(quality / 8)};
    }
    case PlayingLedEffect::FixedBreathe: {
      const RgbColor color = {
          static_cast<uint8_t>(playingLedFixedColor >> 16),
          static_cast<uint8_t>(playingLedFixedColor >> 8),
          static_cast<uint8_t>(playingLedFixedColor)};
      return scaleRgb(color, breathingLevel(now, 4000U));
    }
    case PlayingLedEffect::Temperature: {
      const uint32_t position = now % 12000U;
      const uint8_t amount = position < 6000U
                                 ? position * 255U / 6000U
                                 : (12000U - position) * 255U / 6000U;
      return blendRgb({255, 72, 4}, {155, 210, 255}, amount);
    }
    case PlayingLedEffect::Starlight: {
      const uint32_t interval = now / 1400U;
      const uint32_t position = now % 1400U;
      const uint32_t start = ledHash(interval) % 1050U;
      uint8_t level = 10;
      if (position >= start && position < start + 180U) {
        level = trianglePulse(position, start + 90U, 90U);
      }
      return scaleRgb({175, 215, 255}, level);
    }
    case PlayingLedEffect::Rainbow:
    default:
      return rainbowRgb((now % kPlayingLedRainbowPeriodMs) * 768U /
                        kPlayingLedRainbowPeriodMs);
  }
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
  if (mode == StatusLedMode::Buffering) {
    constexpr uint32_t periodMs = 4000;
    constexpr uint32_t halfPeriodMs = periodMs / 2;
    const uint32_t position = now % periodMs;
    const uint16_t ramp = position < halfPeriodMs
                              ? position * 255U / halfPeriodMs
                              : (periodMs - position) * 255U / halfPeriodMs;
    const uint8_t level = 1U + static_cast<uint32_t>(ramp) * ramp *
                                  (kStatusLedBrightness - 1U) / 65025U;
    red = level;
  } else if (mode == StatusLedMode::Playing) {
    const RgbColor color =
        scaleRgb(renderPlayingLedEffect(now), kPlayingLedBrightness);
    red = color.red;
    green = color.green;
    blue = color.blue;
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
  for (uint16_t stationIndex = 0; stationIndex < stationCount; ++stationIndex) {
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
  for (uint16_t stored = 0; stored < stationCount; ++stored) {
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

bool importExpandedStationPack() {
  Preferences importPreferences;
  importPreferences.begin("catalog", false);
  const bool imported = importPreferences.getBool(kExpandedStationPackKey, false);
  importPreferences.end();
  if (imported) return false;

  bool complete = false;
  const bool changed = importBuiltinRange(kLegacyBuiltinStationCount,
                                          kBuiltinStationCount, complete);
  if (complete) {
    importPreferences.begin("catalog", false);
    importPreferences.putBool(kExpandedStationPackKey, true);
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
  for (uint16_t index = 0; index < stationCount; ++index) {
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
  // Requested primary groups: central, Shanghai, Jiangsu, Anhui, Zhejiang,
  // Beijing, foreign stations, then the remaining provincial regions.
  {"CNR", 0}, {"CRI", 0}, {"CCTV", 0}, {"China Plus", 0},
  {"华语环球", 0}, {"世界华声", 0},
  {"上海", 1}, {"第一财经", 1}, {"东广", 1},
  {"江苏", 2}, {"南京", 2}, {"苏州", 2}, {"无锡", 2},
  {"常州", 2}, {"南通", 2}, {"镇江", 2}, {"扬州", 2},
  {"安徽", 3}, {"合肥", 3},
  {"浙江", 4}, {"杭州", 4}, {"宁波", 4}, {"温州", 4},
  {"湖州", 4}, {"绍兴", 4},
  {"北京", 5},
  {"BBC", 6}, {"CNN", 6}, {"CNA", 6}, {"GB News", 6},
  {"LBC", 6}, {"Times Radio", 6}, {"Talk Radio", 6},
  {"NPR", 6}, {"ABC News", 6}, {"Newstalk", 6}, {"Power FM", 6},
  {"Classic FM", 6}, {"MSNBC", 6}, {"Capital FM", 6},
  {"Class FM", 6}, {"Hao FM", 6}, {"Gold FM", 6},
  {"Money FM", 6}, {"Yes FM", 6}, {"Kiss FM", 6},
  {"RFI", 6}, {"新加坡", 6}, {"马来西亚", 6},
  {"天津", 20}, {"重庆", 21}, {"巴渝", 21}, {"万盛", 21},
  {"河北", 22}, {"保定", 22}, {"山西", 23}, {"太原", 23},
  {"阳泉", 23}, {"长治", 23},
  {"内蒙", 24}, {"呼和浩特", 24}, {"呼伦贝尔", 24},
  {"蒙古语", 24}, {"辽宁", 25}, {"沈阳", 25}, {"吉林", 26},
  {"长春", 26}, {"延边", 26}, {"黑龙江", 27}, {"龙江", 27}, {"龙广", 27},
  {"福建", 28}, {"厦门", 28}, {"福州", 28}, {"海峡", 28},
  {"江西", 29}, {"九江", 29}, {"山东", 30}, {"济南", 30},
  {"青岛", 30}, {"潍坊", 30}, {"滨州", 30}, {"阳信", 30},
  {"河南", 31}, {"郑州", 31}, {"鹤壁", 31},
  {"湖北", 32}, {"武汉", 32}, {"楚天", 32}, {"襄阳", 32},
  {"湖南", 33}, {"长沙", 33}, {"芒果", 33}, {"郴州", 33},
  {"衡阳", 33}, {"岳阳", 33}, {"益阳", 33}, {"邵阳", 33},
  {"广东", 34}, {"广州", 34}, {"深圳", 34}, {"珠海", 34},
  {"东莞", 34}, {"佛山", 34}, {"惠州", 34}, {"梅州", 34},
  {"羊城", 34}, {"珠江", 34}, {"花都", 34}, {"西江", 34},
  {"鹤山", 34}, {"南方", 34},
  {"广西", 35}, {"南宁", 35}, {"海南", 36}, {"海口", 36},
  {"四川", 37}, {"成都", 37}, {"绵阳", 37},
  {"贵州", 38}, {"黔西南", 38}, {"云南", 39}, {"昆明", 39},
  {"保山", 39}, {"西藏", 40}, {"拉萨", 40},
  {"陕西", 41}, {"西安", 41}, {"甘肃", 42}, {"青海", 43},
  {"西宁", 43}, {"宁夏", 44}, {"新疆", 45}, {"兵团", 45},
  {"香港", 46}, {"RTHK", 46}, {"凤凰", 46}, {"澳门", 47},
  {"台湾", 48}, {"BCC", 48}, {"飞碟", 48},
};

uint8_t stationRegionOrder(const char *name) {
  for (const RegionPrefix &region : kRegionOrder) {
    if (strncmp(name, region.prefix, strlen(region.prefix)) == 0) return region.order;
  }
  return 250;
}

const char *stationGroupName(const char *name) {
  return stationGroupNameById(stationRegionOrder(name));
}

const char *stationGroupNameById(uint8_t id) {
  switch (id) {
    case 0: return "中央电台";
    case 1: return "上海";
    case 2: return "江苏";
    case 3: return "安徽";
    case 4: return "浙江";
    case 5: return "北京";
    case 6: return "外国电台";
    case 20: return "天津";
    case 21: return "重庆";
    case 22: return "河北";
    case 23: return "山西";
    case 24: return "内蒙古";
    case 25: return "辽宁";
    case 26: return "吉林";
    case 27: return "黑龙江";
    case 28: return "福建";
    case 29: return "江西";
    case 30: return "山东";
    case 31: return "河南";
    case 32: return "湖北";
    case 33: return "湖南";
    case 34: return "广东";
    case 35: return "广西";
    case 36: return "海南";
    case 37: return "四川";
    case 38: return "贵州";
    case 39: return "云南";
    case 40: return "西藏";
    case 41: return "陕西";
    case 42: return "甘肃";
    case 43: return "青海";
    case 44: return "宁夏";
    case 45: return "新疆及兵团";
    case 46: return "香港";
    case 47: return "澳门";
    case 48: return "台湾";
    default: return "其他";
  }
}

bool isKnownStationGroup(uint8_t id) {
  for (uint8_t known : kDefaultStationGroupOrder) {
    if (known == id) return true;
  }
  return false;
}

void resetStationGroupOrderInMemory() {
  memcpy(stationGroupOrder, kDefaultStationGroupOrder,
         sizeof(kDefaultStationGroupOrder));
  stationGroupOrderCount = kStationGroupCapacity;
}

uint8_t stationGroupOrderIndex(uint8_t id) {
  for (uint8_t index = 0; index < stationGroupOrderCount; ++index) {
    if (stationGroupOrder[index] == id) return index;
  }
  return stationGroupOrderCount;
}

bool persistStationGroupOrder() {
  bool present[256] = {};
  for (uint16_t index = 0; index < stationCount; ++index) {
    present[stationRegionOrder(stations[index].name)] = true;
  }
  uint8_t compact[kStationGroupCapacity] = {};
  uint8_t compactCount = 0;
  for (uint8_t index = 0; index < stationGroupOrderCount; ++index) {
    if (present[stationGroupOrder[index]]) {
      compact[compactCount++] = stationGroupOrder[index];
    }
  }
  Preferences catalogPreferences;
  if (!catalogPreferences.begin("catalog", false)) {
    serialLogPrintln(kSerialLogSystemBit,
                     "ERROR: Could not open station group order storage.");
    return false;
  }
  // These one-byte migration markers are obsolete once a valid group-order
  // blob exists. Removing them also releases NVS entries on tightly packed
  // devices upgraded through several catalog versions.
  catalogPreferences.remove(kRegionSortKey);
  catalogPreferences.remove(kGroupLayoutKey);
  size_t written = compactCount > 0
                       ? catalogPreferences.putBytes(kGroupOrderKey, compact,
                                                     compactCount)
                       : 0;
  if (compactCount > 0 && written != compactCount) {
    // Updating an existing blob can temporarily require both the old and new
    // records. Erase it and retry so upgrades with nearly full NVS still have
    // enough room for this small order list. The caller restores the previous
    // order if this retry also fails.
    catalogPreferences.remove(kGroupOrderKey);
    written = catalogPreferences.putBytes(kGroupOrderKey, compact, compactCount);
  }
  const bool saved = compactCount > 0 && written == compactCount;
  catalogPreferences.end();
  if (!saved) {
    serialLogPrintf(kSerialLogSystemBit,
                    "ERROR: Could not write %u-byte station group order to NVS.\n",
                    static_cast<unsigned>(compactCount));
  }
  return saved;
}

void loadStationGroupOrder() {
  resetStationGroupOrderInMemory();
  stationGroupOrderLoaded = false;
  Preferences catalogPreferences;
  if (!catalogPreferences.begin("catalog", true)) return;
  const size_t storedLength = catalogPreferences.getBytesLength(kGroupOrderKey);
  uint8_t stored[kStationGroupCapacity] = {};
  const size_t readLength =
      storedLength > 0 && storedLength <= sizeof(stored)
          ? catalogPreferences.getBytes(kGroupOrderKey, stored, storedLength)
          : 0;
  catalogPreferences.end();
  if (readLength == 0) return;
  stationGroupOrderLoaded = true;

  uint8_t normalized[kStationGroupCapacity] = {};
  uint8_t normalizedCount = 0;
  for (size_t index = 0; index < readLength; ++index) {
    const uint8_t id = stored[index];
    if (!isKnownStationGroup(id)) continue;
    bool duplicate = false;
    for (uint8_t prior = 0; prior < normalizedCount; ++prior) {
      if (normalized[prior] == id) duplicate = true;
    }
    if (!duplicate) normalized[normalizedCount++] = id;
  }
  for (uint8_t id : kDefaultStationGroupOrder) {
    bool present = false;
    for (uint8_t index = 0; index < normalizedCount; ++index) {
      if (normalized[index] == id) present = true;
    }
    if (present || id == kOtherStationGroupId) continue;
    uint8_t insertion = normalizedCount;
    for (uint8_t index = 0; index < normalizedCount; ++index) {
      if (normalized[index] == kOtherStationGroupId) {
        insertion = index;
        break;
      }
    }
    for (uint8_t index = normalizedCount; index > insertion; --index) {
      normalized[index] = normalized[index - 1];
    }
    normalized[insertion] = id;
    ++normalizedCount;
  }
  bool hasOther = false;
  for (uint8_t index = 0; index < normalizedCount; ++index) {
    if (normalized[index] == kOtherStationGroupId) hasOther = true;
  }
  if (!hasOther) normalized[normalizedCount++] = kOtherStationGroupId;
  memcpy(stationGroupOrder, normalized, normalizedCount);
  stationGroupOrderCount = normalizedCount;
}

bool regroupStationsInMemory() {
  if (stationCount < 2) return true;
  Station *ordered = static_cast<Station *>(ps_malloc(stationCount * sizeof(Station)));
  if (ordered == nullptr) return false;

  uint16_t output = 0;
  uint16_t trackedSelection = selectedStation;
  for (uint8_t order = 0; order < stationGroupOrderCount; ++order) {
    const uint8_t groupId = stationGroupOrder[order];
    for (uint16_t input = 0; input < stationCount; ++input) {
      if (stationRegionOrder(stations[input].name) != groupId) continue;
      ordered[output] = stations[input];
      if (input == selectedStation) trackedSelection = output;
      ++output;
    }
  }
  if (output != stationCount) {
    free(ordered);
    return false;
  }
  memcpy(stations, ordered, stationCount * sizeof(Station));
  free(ordered);
  selectedStation = trackedSelection;
  return true;
}

bool stationGroupsAreContiguousAndOrdered() {
  uint8_t previousRank = 0;
  bool first = true;
  for (uint16_t index = 0; index < stationCount; ++index) {
    const uint8_t rank = stationGroupOrderIndex(stationRegionOrder(stations[index].name));
    if (!first && rank < previousRank) return false;
    previousRank = rank;
    first = false;
  }
  return true;
}

void initialiseStationGroups(bool forceRegroup = false) {
  loadStationGroupOrder();
  const bool needsRegroup = forceRegroup || !stationGroupOrderLoaded ||
                            !stationGroupsAreContiguousAndOrdered();
  if (needsRegroup) {
    if (!regroupStationsInMemory() || !persistPlaylist()) return;
  }

  stationGroupOrderLoaded = persistStationGroupOrder();
}

void addLog(const char *kind, const char *message) {
  snprintf(logs[logHead], sizeof(logs[logHead]), "%lus [%s] %.92s",
           static_cast<unsigned long>(millis() / 1000U), kind, message ? message : "");
  logHead = (logHead + 1) % kLogCapacity;
  if (logCount < kLogCapacity) ++logCount;
  if (serialLogKindEnabled(kind)) {
    Serial.println(logs[(logHead + kLogCapacity - 1) % kLogCapacity]);
  }
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
    serialLogPrintf(kSerialLogAudioBit, "audio %s: %s\n", message.s,
                    message.msg);
  }
}

void playBootChime() {
  if (!LittleFS.exists(kBootChimePath)) {
    addLog("boot", "boot chime asset missing");
    return;
  }
  playbackEnabled = false;
  audio.setVolume(kBootChimeVolume);
  if (!audio.connecttoFS(LittleFS, kBootChimePath)) {
    addLog("boot", "boot chime could not start");
    audio.setVolume(playerVolume);
    playbackEnabled = true;
    return;
  }
  addLog("boot", "playing startup chime");
  const uint32_t deadline = millis() + kBootChimeMaxMs;
  while (audio.isRunning() && !timeReached(millis(), deadline)) {
    audio.loop();
    delay(1);
  }
  audio.stopSong();
  audio.setVolume(playerVolume);
  playbackEnabled = true;
  playerRequested = false;
  playbackAwaitingReady = false;
  playbackFaultPending = false;
}

void initialiseAudioOutput() {
  if (audioOutputReady) return;
  // ESP.restart() does not always reset an allocated ESP-IDF I2S peripheral.
  // Reset both S3 I2S modules before Audio allocates its automatic channel so
  // a software restart cannot leave setPinout() with no available channel.
  periph_module_reset(PERIPH_I2S0_MODULE);
  periph_module_reset(PERIPH_I2S1_MODULE);
  audio.setPinout(config::kI2sBclk, config::kI2sLrclk,
                  config::kI2sDataOut);
  audio.setVolume(playerVolume);
  audioOutputReady = true;
}

void loadSecurity() {
  playerPreferences.begin(kSecurityNamespace, true);
  const String stored = playerPreferences.getString(kAdminPasswordKey, "");
  playerPreferences.end();
  strlcpy(adminPassword, stored.c_str(), sizeof(adminPassword));
}

void loadSerialLogSettings() {
  Preferences logPreferences;
  if (!logPreferences.begin(config::kSerialLogNamespace, true)) return;
  serialLogMask = logPreferences.getUChar(config::kSerialLogMaskKey,
                                           kSerialLogSystemBit) &
                  kSerialLogAllBits;
  logPreferences.end();
  touchDebugEnabled = serialLogEnabled(kSerialLogTouchBit);
}

String serialLogSettingsJson() {
  return "{\"system\":" +
         String(serialLogEnabled(kSerialLogSystemBit) ? "true" : "false") +
         ",\"wifi\":" +
         String(serialLogEnabled(kSerialLogWifiBit) ? "true" : "false") +
         ",\"audio\":" +
         String(serialLogEnabled(kSerialLogAudioBit) ? "true" : "false") +
         ",\"touch\":" +
         String(serialLogEnabled(kSerialLogTouchBit) ? "true" : "false") +
         "}";
}

bool serialLogArgumentEnabled(const char *name) {
  if (!server.hasArg(name)) return false;
  const String value = server.arg(name);
  return value == "true" || value == "1" || value == "on";
}

void handleSaveSerialLogSettings() {
  if (!requireAdmin()) return;
  uint8_t nextMask = 0;
  if (serialLogArgumentEnabled("system")) nextMask |= kSerialLogSystemBit;
  if (serialLogArgumentEnabled("wifi")) nextMask |= kSerialLogWifiBit;
  if (serialLogArgumentEnabled("audio")) nextMask |= kSerialLogAudioBit;
  if (serialLogArgumentEnabled("touch")) nextMask |= kSerialLogTouchBit;

  Preferences logPreferences;
  if (!logPreferences.begin(config::kSerialLogNamespace, false)) {
    sendJson("{\"error\":\"could not open serial log settings\"}", 500);
    return;
  }
  const bool saved =
      logPreferences.putUChar(config::kSerialLogMaskKey, nextMask) == 1;
  logPreferences.end();
  if (!saved) {
    sendJson("{\"error\":\"could not save serial log settings\"}", 500);
    return;
  }
  serialLogMask = nextMask;
  touchDebugEnabled = serialLogEnabled(kSerialLogTouchBit);
  sendJson(serialLogSettingsJson());
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

bool parsePlayingLedEffect(const String &id, PlayingLedEffect &effect) {
  for (const PlayingLedEffectOption &option : kPlayingLedEffectOptions) {
    if (id == option.id) {
      effect = option.effect;
      return true;
    }
  }
  return false;
}

const char *playingLedEffectId() {
  for (const PlayingLedEffectOption &option : kPlayingLedEffectOptions) {
    if (playingLedEffect == option.effect) return option.id;
  }
  return "rainbow";
}

void loadLedSettings() {
  Preferences ledPreferences;
  if (!ledPreferences.begin(kLedNamespace, true)) return;
  const String effectId =
      ledPreferences.getString("effect", playingLedEffectId());
  const uint32_t fixedColor =
      ledPreferences.getUInt("fixed_color", playingLedFixedColor);
  ledPreferences.end();
  PlayingLedEffect effect;
  if (parsePlayingLedEffect(effectId, effect)) playingLedEffect = effect;
  playingLedFixedColor = fixedColor & 0xFFFFFFU;
}

String ledSettingsJson() {
  char color[8];
  snprintf(color, sizeof(color), "#%06lX",
           static_cast<unsigned long>(playingLedFixedColor));
  return "{\"effect\":\"" + String(playingLedEffectId()) +
         "\",\"fixed_color\":\"" + String(color) + "\"}";
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

void handleSaveLedSettings() {
  if (!requireAdmin()) return;
  const String effectId = server.arg("effect");
  const String fixedColorText = server.arg("fixed_color");
  PlayingLedEffect effect;
  if (!parsePlayingLedEffect(effectId, effect) ||
      !isHexColor(fixedColorText)) {
    sendJson("{\"error\":\"invalid LED settings\"}", 400);
    return;
  }
  const uint32_t fixedColor =
      strtoul(fixedColorText.c_str() + 1, nullptr, 16) & 0xFFFFFFU;
  Preferences ledPreferences;
  if (!ledPreferences.begin(kLedNamespace, false)) {
    sendJson("{\"error\":\"could not open LED settings\"}", 500);
    return;
  }
  const bool saved =
      ledPreferences.putString("effect", effectId) == effectId.length() &&
      ledPreferences.putUInt("fixed_color", fixedColor) == sizeof(uint32_t);
  ledPreferences.end();
  if (!saved) {
    sendJson("{\"error\":\"could not save LED settings\"}", 500);
    return;
  }
  playingLedEffect = effect;
  playingLedFixedColor = fixedColor;
  audioOutputPeak = 0;
  updateStatusLed(true);
  sendJson(ledSettingsJson());
}

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
  // Delay I2S allocation until playback is actually needed. This keeps the
  // provisioning AP and management UI alive if an earlier reset left I2S busy.
  initialiseAudioOutput();
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

void onPlaylistSelectionV8(uint16_t) {
  playbackEnabled = true;
  stationChangePending = true;
  pendingPlaybackReason = "station selected";
  playerRequested = true;
  playbackAwaitingReady = false;
  playbackFaultPending = false;
  statusLedError = false;
  setPlayerMessage("switching station");
}

void finishWifiRecovery() {
  wifiRecoveryState = WifiRecoveryState::Idle;
  wifiRecoveryCandidateCount = 0;
  wifiRecoveryCandidateIndex = 0;
  wifiRecoveryAttemptStartedAt = 0;
  lastWifiRetryAt = millis();
}

void startNextWifiRecoveryCandidate() {
  if (wifiRecoveryCandidateIndex >= wifiRecoveryCandidateCount) {
    addLog("wifi", "no saved network connected");
    finishWifiRecovery();
    return;
  }
  const int candidate = wifiRecoveryCandidates[wifiRecoveryCandidateIndex];
  serialLogPrintf(kSerialLogWifiBit, "Wi-Fi recovery: trying %s\n",
                  savedWifiNetworks[candidate].ssid);
  WiFi.disconnect(false, false);
  WiFi.begin(savedWifiNetworks[candidate].ssid, savedWifiNetworks[candidate].password);
  wifiRecoveryAttemptStartedAt = millis();
  wifiRecoveryState = WifiRecoveryState::Connecting;
}

void buildWifiRecoveryCandidates(int scanCount) {
  wifiRecoveryCandidateCount = 0;
  wifiRecoveryCandidateIndex = 0;
  bool selected[config::kMaxSavedWifiNetworks] = {};
  // Match visible SSIDs first, strongest signal first.
  for (uint8_t rank = 0; rank < savedWifiNetworkCount; ++rank) {
    int best = -1;
    int32_t bestRssi = -127;
    for (uint8_t saved = 0; saved < savedWifiNetworkCount; ++saved) {
      if (selected[saved]) continue;
      for (int found = 0; found < scanCount; ++found) {
        if (WiFi.SSID(found) == savedWifiNetworks[saved].ssid &&
            WiFi.RSSI(found) > bestRssi) {
          best = saved;
          bestRssi = WiFi.RSSI(found);
        }
      }
    }
    if (best < 0) break;
    selected[best] = true;
    wifiRecoveryCandidates[wifiRecoveryCandidateCount++] = best;
  }
  // A hidden SSID does not appear in scans, but remains a valid fallback.
  for (uint8_t saved = 0; saved < savedWifiNetworkCount; ++saved) {
    if (!selected[saved]) {
      wifiRecoveryCandidates[wifiRecoveryCandidateCount++] = saved;
    }
  }
}

void beginWifiRecovery() {
  if (wifiScanInProgress || wifiRecoveryState != WifiRecoveryState::Idle) return;
  if (!loadSavedWifiNetworks() || savedWifiNetworkCount == 0) {
    finishWifiRecovery();
    return;
  }
  stationConfigured = true;
  WiFi.mode(accessPointRunning ? WIFI_AP_STA : WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.scanDelete();
  const int result = WiFi.scanNetworks(true, true);
  if (result == WIFI_SCAN_FAILED) {
    addLog("wifi", "recovery scan could not start");
    finishWifiRecovery();
    return;
  }
  wifiRecoveryState = WifiRecoveryState::Scanning;
  lastWifiRetryAt = millis();
  addLog("wifi", "recovery scan started");
}

void maintainWifiRecovery() {
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiRecoveryState != WifiRecoveryState::Idle) {
      addLog("wifi", "saved network connected");
      WiFi.setAutoReconnect(true);
      finishWifiRecovery();
    }
    return;
  }
  if (wifiScanInProgress) return;

  if (wifiRecoveryState == WifiRecoveryState::Idle) {
    if (millis() - lastWifiRetryAt >= kWifiRetryMs) beginWifiRecovery();
    return;
  }
  if (wifiRecoveryState == WifiRecoveryState::Scanning) {
    const int scanCount = WiFi.scanComplete();
    if (scanCount == WIFI_SCAN_RUNNING) return;
    if (scanCount < 0) {
      addLog("wifi", "recovery scan failed");
      WiFi.scanDelete();
      finishWifiRecovery();
      return;
    }
    buildWifiRecoveryCandidates(scanCount);
    WiFi.scanDelete();
    startNextWifiRecoveryCandidate();
    return;
  }
  if (millis() - wifiRecoveryAttemptStartedAt >=
      config::kWifiConnectAttemptTimeoutMs) {
    ++wifiRecoveryCandidateIndex;
    startNextWifiRecoveryCandidate();
  }
}

void maintainNetworkAndPlayback() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected) maintainWifiRecovery();
  if (!connected) {
    if (wasStationConnected) {
      audio.stopSong();
      playerRequested = false;
      playbackAwaitingReady = false;
      playbackFaultPending = false;
      setPlayerMessage("router Wi-Fi disconnected; retrying");
      addLog("wifi", "router Wi-Fi disconnected");
    }
    maintainWifiRecovery();
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
  const uint16_t previousSelection = selectedStation;
  selectedStation = adjacentStationIndex(true);
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
  const uint16_t previousSelection = selectedStation;
  selectedStation = adjacentStationIndex(false);
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

bool selectStationForUser(uint16_t id, const char *reason) {
  const uint16_t previousSelection = selectedStation;
  selectedStation = id;
  if (!persistSelectedStation()) {
    selectedStation = previousSelection;
    return false;
  }
  requestUserPlayback(reason);
  return true;
}

struct TouchButton {
  uint8_t pin;
  uint32_t baseline = 0;
  uint32_t lastValue = 0;
  uint8_t consecutiveSamples = 0;
  bool pressed = false;
  bool ready = false;
};

TouchButton previousTouch{config::kPreviousTouchPin};
TouchButton nextTouch{config::kNextTouchPin};
TouchButton playPauseTouch{config::kPlayPauseTouchPin};
uint32_t lastTouchScanAt = 0;
uint32_t lastTouchDebugAt = 0;
uint8_t touchSensitivityPercent = config::kDefaultTouchSensitivityPercent;
bool touchCalibrationInProgress = false;
uint8_t touchCalibrationSampleCount = 0;
uint64_t previousTouchCalibrationTotal = 0;
uint64_t nextTouchCalibrationTotal = 0;
uint64_t playPauseTouchCalibrationTotal = 0;
char serialCommandBuffer[config::kSerialCommandBufferSize] = {};
size_t serialCommandLength = 0;

uint32_t touchPressThreshold(const TouchButton &button) {
  const uint32_t margin = max<uint32_t>(
      button.baseline * touchSensitivityPercent / 100U, 300U);
  return button.baseline + margin;
}

uint32_t touchReleaseThreshold(const TouchButton &button) {
  const uint8_t releasePercent = max<uint8_t>(touchSensitivityPercent / 2U, 1U);
  const uint32_t margin =
      max<uint32_t>(button.baseline * releasePercent / 100U, 150U);
  return button.baseline + margin;
}

void printTouchButtonStatus(const char *name, const TouchButton &button) {
  Serial.printf(
      "TOUCH %-8s gpio=%u raw=%lu baseline=%lu press=%lu release=%lu "
      "state=%s ready=%s\n",
      name, button.pin, static_cast<unsigned long>(button.lastValue),
      static_cast<unsigned long>(button.baseline),
      static_cast<unsigned long>(touchPressThreshold(button)),
      static_cast<unsigned long>(touchReleaseThreshold(button)),
      button.pressed ? "pressed" : "released",
      button.ready ? "yes" : "no");
}

void printTouchStatus() {
  Serial.printf("TOUCH sensitivity=%u%% (lower value is more sensitive)\n",
                touchSensitivityPercent);
  if (touchCalibrationInProgress) {
    Serial.printf("TOUCH calibration %u/%u\n", touchCalibrationSampleCount,
                  config::kTouchCalibrationSamples);
  }
  printTouchButtonStatus("previous", previousTouch);
  printTouchButtonStatus("next", nextTouch);
  printTouchButtonStatus("play", playPauseTouch);
}

void calibrateTouchButton(TouchButton &button) {
  uint64_t total = 0;
  for (uint8_t sample = 0; sample < config::kTouchCalibrationSamples;
       ++sample) {
    total += touchRead(button.pin);
    delay(5);
  }
  button.baseline = static_cast<uint32_t>(
      total / config::kTouchCalibrationSamples);
  button.lastValue = button.baseline;
  button.ready = button.baseline > 0;
  serialLogPrintf(kSerialLogTouchBit, "Touch GPIO%u baseline: %lu (%s)\n",
                  button.pin, static_cast<unsigned long>(button.baseline),
                  button.ready ? "ready" : "unavailable");
}

void initialiseTouchButtons() {
  // Keep all three electrodes untouched during this short startup calibration.
  calibrateTouchButton(previousTouch);
  calibrateTouchButton(nextTouch);
  calibrateTouchButton(playPauseTouch);
}

void beginTouchCalibration() {
  touchCalibrationInProgress = true;
  touchCalibrationSampleCount = 0;
  previousTouchCalibrationTotal = 0;
  nextTouchCalibrationTotal = 0;
  playPauseTouchCalibrationTotal = 0;
  previousTouch.ready = false;
  nextTouch.ready = false;
  playPauseTouch.ready = false;
  previousTouch.pressed = false;
  nextTouch.pressed = false;
  playPauseTouch.pressed = false;
  previousTouch.consecutiveSamples = 0;
  nextTouch.consecutiveSamples = 0;
  playPauseTouch.consecutiveSamples = 0;
  Serial.println("TOUCH calibration started; release all three electrodes");
}

void sampleTouchCalibration() {
  previousTouch.lastValue = touchRead(previousTouch.pin);
  nextTouch.lastValue = touchRead(nextTouch.pin);
  playPauseTouch.lastValue = touchRead(playPauseTouch.pin);
  previousTouchCalibrationTotal += previousTouch.lastValue;
  nextTouchCalibrationTotal += nextTouch.lastValue;
  playPauseTouchCalibrationTotal += playPauseTouch.lastValue;
  ++touchCalibrationSampleCount;
  if (touchCalibrationSampleCount < config::kTouchCalibrationSamples) return;

  previousTouch.baseline = static_cast<uint32_t>(
      previousTouchCalibrationTotal / config::kTouchCalibrationSamples);
  nextTouch.baseline = static_cast<uint32_t>(
      nextTouchCalibrationTotal / config::kTouchCalibrationSamples);
  playPauseTouch.baseline = static_cast<uint32_t>(
      playPauseTouchCalibrationTotal / config::kTouchCalibrationSamples);
  previousTouch.lastValue = previousTouch.baseline;
  nextTouch.lastValue = nextTouch.baseline;
  playPauseTouch.lastValue = playPauseTouch.baseline;
  previousTouch.ready = previousTouch.baseline > 0;
  nextTouch.ready = nextTouch.baseline > 0;
  playPauseTouch.ready = playPauseTouch.baseline > 0;
  touchCalibrationInProgress = false;
  Serial.println("TOUCH calibration complete");
  printTouchStatus();
}

bool updateTouchButton(TouchButton &button) {
  if (!button.ready) return false;

  const uint32_t value = touchRead(button.pin);
  button.lastValue = value;
  const uint32_t pressThreshold = touchPressThreshold(button);
  const uint32_t releaseThreshold = touchReleaseThreshold(button);

  if (!button.pressed) {
    // Follow slow temperature and humidity drift, but never learn a touch as
    // the new idle baseline.
    if (value < button.baseline + button.baseline / 10U) {
      button.baseline = static_cast<uint32_t>(
          (static_cast<uint64_t>(button.baseline) * 63U + value) / 64U);
    }
    if (value >= pressThreshold) {
      if (++button.consecutiveSamples >= config::kTouchDebounceSamples) {
        button.consecutiveSamples = 0;
        button.pressed = true;
        return true;
      }
    } else {
      button.consecutiveSamples = 0;
    }
  } else if (value <= releaseThreshold) {
    if (++button.consecutiveSamples >= config::kTouchDebounceSamples) {
      button.consecutiveSamples = 0;
      button.pressed = false;
    }
  } else {
    button.consecutiveSamples = 0;
  }
  return false;
}

void pollTouchButtons() {
  const uint32_t now = millis();
  if (now - lastTouchScanAt < config::kTouchScanIntervalMs) return;
  lastTouchScanAt = now;

  if (touchCalibrationInProgress) {
    sampleTouchCalibration();
    return;
  }

  const bool previousPressed = updateTouchButton(previousTouch);
  const bool nextPressed = updateTouchButton(nextTouch);
  const bool playPausePressed = updateTouchButton(playPauseTouch);
  const uint8_t pressedCount = static_cast<uint8_t>(previousPressed) +
                               static_cast<uint8_t>(nextPressed) +
                               static_cast<uint8_t>(playPausePressed);
  // Ignore simultaneous pads rather than triggering more than one action.
  const bool anotherPadHeld =
      (previousPressed && (nextTouch.pressed || playPauseTouch.pressed)) ||
      (nextPressed && (previousTouch.pressed || playPauseTouch.pressed)) ||
      (playPausePressed && (previousTouch.pressed || nextTouch.pressed));
  if (pressedCount != 1 || anotherPadHeld || stationCount == 0) return;

  if (playPausePressed) {
    if (touchDebugEnabled) {
      Serial.printf("TOUCH event=%s raw=%lu\n",
                    playbackEnabled ? "pause" : "play",
                    static_cast<unsigned long>(playPauseTouch.lastValue));
    }
    if (playbackEnabled) {
      playbackEnabled = false;
      stationChangePending = false;
      playbackAwaitingReady = false;
      playbackFaultPending = false;
      statusLedError = false;
      audio.stopSong();
      playerRequested = false;
      setPlayerMessage("stopped by touch");
      addLog("touch", "playback paused");
    } else {
      requestUserPlayback("play requested from touch");
      addLog("touch", "playback resumed");
    }
    return;
  }

  const uint16_t target = adjacentStationIndex(previousPressed);
  const char *reason = previousPressed ? "previous station from touch"
                                       : "next station from touch";
  if (touchDebugEnabled) {
    Serial.printf("TOUCH event=%s station=%u raw=%lu\n",
                  previousPressed ? "previous" : "next", target,
                  static_cast<unsigned long>(previousPressed
                                                 ? previousTouch.lastValue
                                                 : nextTouch.lastValue));
  }
  if (!selectStationForUser(target, reason)) {
    addLog("touch", "could not save selected station");
    return;
  }
  addLog("touch", previousPressed ? "previous station" : "next station");
}

void handleSerialCommand(const char *command) {
  if (strcmp(command, "help") == 0) {
    Serial.println(
        "Commands: touch | touch on | touch off | touch calibrate | "
        "touch sensitivity <3..50>");
  } else if (strcmp(command, "touch") == 0 ||
             strcmp(command, "touch status") == 0) {
    printTouchStatus();
  } else if (strcmp(command, "touch on") == 0) {
    touchDebugEnabled = true;
    lastTouchDebugAt = millis();
    Serial.println("TOUCH continuous debug enabled (500 ms)");
    printTouchStatus();
  } else if (strcmp(command, "touch off") == 0) {
    touchDebugEnabled = false;
    Serial.println("TOUCH continuous debug disabled");
  } else if (strcmp(command, "touch calibrate") == 0) {
    beginTouchCalibration();
  } else if (strncmp(command, "touch sensitivity ", 18) == 0) {
    const char *valueText = command + 18;
    uint16_t value = 0;
    bool valid = valueText[0] != '\0';
    for (size_t index = 0; valid && valueText[index] != '\0'; ++index) {
      const char character = valueText[index];
      if (character < '0' || character > '9') {
        valid = false;
      } else {
        value = value * 10U + static_cast<uint8_t>(character - '0');
        if (value > config::kMaximumTouchSensitivityPercent) valid = false;
      }
    }
    if (!valid || value < config::kMinimumTouchSensitivityPercent) {
      Serial.println("TOUCH sensitivity must be 3..50 percent");
    } else {
      touchSensitivityPercent = static_cast<uint8_t>(value);
      Serial.printf("TOUCH sensitivity set to %u%%\n",
                    touchSensitivityPercent);
      printTouchStatus();
    }
  } else {
    Serial.printf("Unknown command: %s (type 'help')\n", command);
  }
}

void pollSerialCommands() {
  while (Serial.available() > 0) {
    char character = static_cast<char>(Serial.read());
    if (character == '\r' || character == '\n') {
      if (serialCommandLength == 0) continue;
      serialCommandBuffer[serialCommandLength] = '\0';
      handleSerialCommand(serialCommandBuffer);
      serialCommandLength = 0;
      continue;
    }
    if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
    if (character < 0x20 || character > 0x7e) continue;
    if (serialCommandLength + 1 < config::kSerialCommandBufferSize) {
      serialCommandBuffer[serialCommandLength++] = character;
    } else {
      serialCommandLength = 0;
      Serial.println("Serial command is too long");
    }
  }
}

void printTouchDebugIfDue() {
  if (!touchDebugEnabled) return;
  const uint32_t now = millis();
  if (now - lastTouchDebugAt < config::kTouchDebugIntervalMs) return;
  lastTouchDebugAt = now;
  printTouchStatus();
}

void handleUserSelectStation() {
  uint16_t id;
  if (!parseStationId(id)) {
    sendJson("{\"error\":\"invalid station id\"}", 400);
    return;
  }
  const bool previousFavoriteContext = favoritePlaybackContext;
  favoritePlaybackContext = server.arg("context") == "favorites" &&
                            favoriteStationPosition(id) >= 0;
  if (!selectStationForUser(id, "station selected from user page")) {
    favoritePlaybackContext = previousFavoriteContext;
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
  const uint16_t target = adjacentStationIndex(true);
  if (!selectStationForUser(target, "previous station from user page")) {
    sendJson("{\"error\":\"could not save selected station\"}", 500);
    return;
  }
  sendUserPlayerStatus();
}

void handleUserNext() {
  if (stationCount == 0) { sendJson("{\"error\":\"playlist is empty\"}", 409); return; }
  const uint16_t target = adjacentStationIndex(false);
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
  uint16_t id;
  if (!parseStationId(id)) {
    sendJson("{\"error\":\"invalid station id\"}", 400);
    return;
  }

  const String direction = server.arg("direction");
  const uint8_t groupId = stationRegionOrder(stations[id].name);
  uint16_t groupFirst = id;
  uint16_t groupLast = id;
  while (groupFirst > 0 &&
         stationRegionOrder(stations[groupFirst - 1].name) == groupId) {
    --groupFirst;
  }
  while (groupLast + 1 < stationCount &&
         stationRegionOrder(stations[groupLast + 1].name) == groupId) {
    ++groupLast;
  }
  uint16_t target = id;
  if (direction == "up" && id > groupFirst) target = id - 1;
  else if (direction == "down" && id < groupLast) target = id + 1;
  else if (direction == "first") target = groupFirst;
  else if (direction == "last") target = groupLast;
  else if (direction != "up" && direction != "down") {
    sendJson("{\"error\":\"invalid move direction\"}", 400);
    return;
  } else {
    sendJson("{\"error\":\"station cannot cross its group boundary\"}", 409);
    return;
  }

  if (target == id) {
    sendPlaylistJson(true);
    return;
  }

  const uint16_t previousSelection = selectedStation;
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
      for (uint16_t index = id; index < target; ++index) stations[index] = stations[index + 1];
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
      for (uint16_t index = target; index < id; ++index) {
        stations[index] = stations[index + 1];
      }
    } else {
      for (uint16_t index = target; index > id; --index) {
        stations[index] = stations[index - 1];
      }
    }
    stations[id] = moved;
    selectedStation = previousSelection;
  }
  if (saved) sendPlaylistJson(true);
  else sendJson("{\"error\":\"could not save playlist\"}", 500);
}

void handleSetFavoriteStation() {
  uint16_t id;
  if (!parseStationId(id)) {
    sendJson("{\"error\":\"invalid station id\"}", 400);
    return;
  }
  const String action = server.arg("action");
  if (action != "add" && action != "remove") {
    sendJson("{\"error\":\"invalid favorite action\"}", 400);
    return;
  }

  const int current = favoriteStationPosition(id);
  if ((action == "add" && current >= 0) ||
      (action == "remove" && current < 0)) {
    sendPlaylistJson(true);
    return;
  }
  uint32_t previous[config::kMaxStations] = {};
  memcpy(previous, favoriteStationKeys,
         favoriteStationCount * sizeof(uint32_t));
  const uint16_t previousCount = favoriteStationCount;
  if (action == "add") {
    favoriteStationKeys[favoriteStationCount++] =
        stationFavoriteKey(stations[id]);
  } else {
    for (uint16_t index = current; index + 1 < favoriteStationCount; ++index) {
      favoriteStationKeys[index] = favoriteStationKeys[index + 1];
    }
    --favoriteStationCount;
    favoriteStationKeys[favoriteStationCount] = 0;
  }
  if (!persistFavoriteStations()) {
    memcpy(favoriteStationKeys, previous,
           previousCount * sizeof(uint32_t));
    favoriteStationCount = previousCount;
    sendJson("{\"error\":\"could not save favorites\"}", 500);
    return;
  }
  if (action == "remove" && id == selectedStation) {
    favoritePlaybackContext = false;
    if (!persistSelectedStation()) {
      serialLogPrintln(kSerialLogSystemBit,
                       "WARN: Could not clear favorite playback context.");
    }
  }
  markPlaylistChanged();
  sendPlaylistJson(true);
}

void handleMoveFavoriteStation() {
  uint16_t id;
  if (!parseStationId(id)) {
    sendJson("{\"error\":\"invalid station id\"}", 400);
    return;
  }
  const int currentValue = favoriteStationPosition(id);
  if (currentValue < 0) {
    sendJson("{\"error\":\"station is not a favorite\"}", 409);
    return;
  }
  const String direction = server.arg("direction");
  const uint16_t current = static_cast<uint16_t>(currentValue);
  uint16_t target = current;
  if (direction == "first") target = 0;
  else if (direction == "up" && current > 0) target = current - 1;
  else if (direction == "down" && current + 1 < favoriteStationCount) {
    target = current + 1;
  } else if (direction == "last") target = favoriteStationCount - 1;
  else if (direction != "up" && direction != "down") {
    sendJson("{\"error\":\"invalid move direction\"}", 400);
    return;
  } else {
    sendJson("{\"error\":\"favorite cannot move farther\"}", 409);
    return;
  }
  if (target == current) {
    sendPlaylistJson(true);
    return;
  }

  const uint32_t moved = favoriteStationKeys[current];
  if (target < current) {
    for (uint16_t index = current; index > target; --index) {
      favoriteStationKeys[index] = favoriteStationKeys[index - 1];
    }
  } else {
    for (uint16_t index = current; index < target; ++index) {
      favoriteStationKeys[index] = favoriteStationKeys[index + 1];
    }
  }
  favoriteStationKeys[target] = moved;
  if (!persistFavoriteStations()) {
    if (target < current) {
      for (uint16_t index = target; index < current; ++index) {
        favoriteStationKeys[index] = favoriteStationKeys[index + 1];
      }
    } else {
      for (uint16_t index = target; index > current; --index) {
        favoriteStationKeys[index] = favoriteStationKeys[index - 1];
      }
    }
    favoriteStationKeys[current] = moved;
    sendJson("{\"error\":\"could not save favorite order\"}", 500);
    return;
  }
  markPlaylistChanged();
  sendPlaylistJson(true);
}

bool parseStationGroupId(uint8_t &id) {
  if (!server.hasArg("id")) return false;
  const String value = server.arg("id");
  if (value.isEmpty()) return false;
  uint16_t parsed = 0;
  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value[index];
    if (character < '0' || character > '9') return false;
    parsed = parsed * 10U + static_cast<uint8_t>(character - '0');
    if (parsed > UINT8_MAX) return false;
  }
  id = static_cast<uint8_t>(parsed);
  if (!isKnownStationGroup(id)) return false;
  for (uint16_t index = 0; index < stationCount; ++index) {
    if (stationRegionOrder(stations[index].name) == id) return true;
  }
  return false;
}

enum class StationGroupSaveResult : uint8_t {
  Ok,
  MemoryFailure,
  PlaylistFailure,
  OrderFailure,
};

StationGroupSaveResult applyStationGroupOrderTransaction(
    const uint8_t *previousOrder, uint8_t previousCount) {
  if (!regroupStationsInMemory()) {
    memcpy(stationGroupOrder, previousOrder, previousCount);
    stationGroupOrderCount = previousCount;
    return StationGroupSaveResult::MemoryFailure;
  }
  bool playlistSaved = persistPlaylist();
  if (!playlistSaved && playlistStorageReady) {
    serialLogPrintln(kSerialLogSystemBit,
                     "WARN: Retrying station group playlist snapshot.");
    playlistFileStoreUnavailable = false;
    playlistSaved = persistPlaylist();
  }
  if (!playlistSaved) {
    memcpy(stationGroupOrder, previousOrder, previousCount);
    stationGroupOrderCount = previousCount;
    regroupStationsInMemory();
    return StationGroupSaveResult::PlaylistFailure;
  }
  if (persistStationGroupOrder()) {
    stationGroupOrderLoaded = true;
    return StationGroupSaveResult::Ok;
  }

  memcpy(stationGroupOrder, previousOrder, previousCount);
  stationGroupOrderCount = previousCount;
  regroupStationsInMemory();
  persistPlaylist();
  persistStationGroupOrder();
  return StationGroupSaveResult::OrderFailure;
}

void sendStationGroupSaveError(StationGroupSaveResult result) {
  switch (result) {
    case StationGroupSaveResult::MemoryFailure:
      sendJson("{\"error\":\"内存不足，未调整分组顺序\"}", 500);
      break;
    case StationGroupSaveResult::PlaylistFailure:
      sendJson("{\"error\":\"LittleFS 播放列表保存失败，已恢复原顺序；请查看诊断日志\"}", 500);
      break;
    case StationGroupSaveResult::OrderFailure:
      sendJson("{\"error\":\"NVS 分组顺序保存失败，已恢复原顺序；请查看诊断日志\"}", 500);
      break;
    default:
      break;
  }
}

void handleMoveStationGroup() {
  uint8_t groupId;
  if (!parseStationGroupId(groupId)) {
    sendJson("{\"error\":\"invalid or empty station group\"}", 400);
    return;
  }
  const String direction = server.arg("direction");
  if (direction != "first" && direction != "up" &&
      direction != "down" && direction != "last") {
    sendJson("{\"error\":\"invalid move direction\"}", 400);
    return;
  }

  bool present[256] = {};
  for (uint16_t index = 0; index < stationCount; ++index) {
    present[stationRegionOrder(stations[index].name)] = true;
  }
  const uint8_t current = stationGroupOrderIndex(groupId);
  uint8_t target = current;
  if (direction == "first") {
    for (uint8_t index = 0; index < stationGroupOrderCount; ++index) {
      if (present[stationGroupOrder[index]]) { target = index; break; }
    }
  } else if (direction == "last") {
    for (int index = stationGroupOrderCount - 1; index >= 0; --index) {
      if (present[stationGroupOrder[index]]) {
        target = static_cast<uint8_t>(index);
        break;
      }
    }
  } else if (direction == "up") {
    for (int index = current - 1; index >= 0; --index) {
      if (present[stationGroupOrder[index]]) {
        target = static_cast<uint8_t>(index);
        break;
      }
    }
  } else {
    for (uint8_t index = current + 1; index < stationGroupOrderCount; ++index) {
      if (present[stationGroupOrder[index]]) { target = index; break; }
    }
  }
  if (target == current) {
    sendJson("{\"moved\":false,\"revision\":" + String(playlistRevision) + "}");
    return;
  }

  uint8_t previousOrder[kStationGroupCapacity] = {};
  memcpy(previousOrder, stationGroupOrder, stationGroupOrderCount);
  const uint8_t previousCount = stationGroupOrderCount;
  if (target < current) {
    for (uint8_t index = current; index > target; --index) {
      stationGroupOrder[index] = stationGroupOrder[index - 1];
    }
  } else {
    for (uint8_t index = current; index < target; ++index) {
      stationGroupOrder[index] = stationGroupOrder[index + 1];
    }
  }
  stationGroupOrder[target] = groupId;

  const StationGroupSaveResult result =
      applyStationGroupOrderTransaction(previousOrder, previousCount);
  if (result != StationGroupSaveResult::Ok) {
    sendStationGroupSaveError(result);
    return;
  }
  sendJson("{\"moved\":true,\"revision\":" + String(playlistRevision) + "}");
}

void handleResetStationGroups() {
  uint8_t previousOrder[kStationGroupCapacity] = {};
  memcpy(previousOrder, stationGroupOrder, stationGroupOrderCount);
  const uint8_t previousCount = stationGroupOrderCount;
  resetStationGroupOrderInMemory();
  const StationGroupSaveResult result =
      applyStationGroupOrderTransaction(previousOrder, previousCount);
  if (result != StationGroupSaveResult::Ok) {
    sendStationGroupSaveError(result);
    return;
  }
  sendJson("{\"reset\":true,\"revision\":" + String(playlistRevision) + "}");
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
                              "player", "catalog", kSecurityNamespace,
                              kUiNamespace, kLedNamespace,
                              config::kSerialLogNamespace};
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

// LittleFS uses the partition-table subtype named "spiffs" in Arduino ESP32,
// so U_SPIFFS deliberately targets the LittleFS partition at 0x620000.
// This replaces only web assets and persistent LittleFS files; NVS settings
// (Wi-Fi, stations, password, and theme) are left intact.
void handleResourceOtaUpload() {
  if (!isAdminRequest()) return;
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    otaSucceeded = false;
    otaInProgress = true;
    playbackWasEnabledBeforeOta = playbackEnabled;
    statusLedError = false;
    otaError = "";
    playbackEnabled = false;
    stationChangePending = false;
    playbackAwaitingReady = false;
    playbackFaultPending = false;
    audio.stopSong();
    playerRequested = false;
    addLog("ota", "LittleFS resource upload started");
    updateStatusLed(true);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)) {
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
    addLog("ota", otaSucceeded ? "LittleFS resources verified" : otaError.c_str());
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    failOtaUpload("LittleFS resource upload aborted");
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
:root{color-scheme:dark;--bg:#656b6a;--panel:#707675;--text:#fff;--muted:#d7dcda;--line:#858b89;--accent:#f2a51a}*{box-sizing:border-box}body{margin:0;background:#4e5453;color:var(--text);font-family:-apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif}.app{position:relative;width:100%;max-width:720px;min-height:100vh;margin:auto;padding:20px clamp(18px,5vw,42px) 50px;background:var(--bg);box-shadow:0 0 32px #0004}.settings{position:absolute;right:18px;top:16px;display:grid;place-items:center;width:48px;height:48px;border:0;border-radius:50%;background:#ffffff1c;color:#fff;text-decoration:none;font-size:27px}.settings:active{transform:scale(.96)}.hero{text-align:center;padding-top:58px}.cover-wrap{position:relative;width:min(48vw,250px);aspect-ratio:1;margin:auto;border-radius:22px;background:#f5f5f5;overflow:hidden;box-shadow:0 8px 25px #0003}.cover{display:block;width:100%;height:100%;object-fit:contain;object-position:center}.cover-fallback{position:absolute;inset:0;display:none;place-items:center;background:linear-gradient(145deg,#f5a623,#d47b13);font-size:clamp(46px,12vw,78px);font-weight:800}.station-name{min-height:1.5em;margin:25px 0 5px;font-size:clamp(25px,5vw,34px);font-weight:700}.state{color:var(--accent);font-size:18px}.progress{height:7px;margin:34px 0 28px;background:#a7adaa;border-radius:10px;overflow:hidden}.progress i{display:block;width:0;height:100%;background:var(--accent);transition:width .4s}.progress.busy i{width:58%;animation:load 1.5s ease-in-out infinite}@keyframes load{0%{transform:translateX(-110%)}100%{transform:translateX(180%)}}.controls{display:flex;align-items:center;justify-content:space-around;max-width:530px;margin:auto}.controls button{display:grid;place-items:center;border:0;color:#fff;background:transparent;cursor:pointer}.controls button:not(.play){width:80px;height:70px;font-size:42px}.controls .play{width:108px;height:108px;border-radius:50%;background:#fff;color:#5e6463;font-size:48px;box-shadow:0 7px 22px #0003}.volume{display:flex;align-items:center;gap:13px;margin:30px 4px 24px;color:var(--muted)}input[type=range]{width:100%;accent-color:var(--accent)}.list-title{display:flex;align-items:center;justify-content:space-between;margin:15px 0 5px}.list-title h2{font-size:18px;margin:0}.count{color:var(--muted);font-size:14px}.station-list{border-top:1px solid var(--line)}.station-group{padding:20px 10px 7px;color:var(--accent);font-size:15px;font-weight:800;border-bottom:1px solid var(--line)}.station{display:flex;align-items:center;gap:17px;width:100%;min-height:88px;padding:12px 10px;border:0;border-bottom:1px solid var(--line);background:transparent;color:#fff;text-align:left;cursor:pointer;content-visibility:auto;contain-intrinsic-size:88px}.station.active{background:#ffffff12;border-left:4px solid var(--accent);padding-left:6px}.station img,.station .fallback{display:block;flex:0 0 62px;width:62px;height:62px;border-radius:13px;background:#f7f7f7;object-fit:contain;object-position:center}.station .fallback{display:grid;place-items:center;background:linear-gradient(145deg,#f5a623,#d47b13);color:#fff;font-size:25px;font-weight:800}.station b{font-size:19px;font-weight:600}.station small{display:block;margin-top:4px;color:var(--muted)}.notice{padding:30px 8px;text-align:center;color:var(--muted)}@media(max-width:480px){.app{padding-left:16px;padding-right:16px}.hero{padding-top:50px}.cover-wrap{width:56vw}.station-name{font-size:25px}.controls .play{width:94px;height:94px}.controls button:not(.play){font-size:34px}.station{min-height:78px;contain-intrinsic-size:78px}.station img,.station .fallback{flex-basis:54px;width:54px;height:54px}}</style></head>
<body><main class="app"><a class="settings" href="/admin" aria-label="进入管理页面" title="设置">⚙</a><section class="hero"><div class="cover-wrap"><img id="cover" class="cover" alt="当前电台台标"><div id="coverFallback" class="cover-fallback">R</div></div><div id="stationName" class="station-name">加载中…</div><div id="state" class="state">正在连接设备</div></section><div id="progress" class="progress"><i></i></div><nav class="controls" aria-label="播放控制"><button id="previous" aria-label="上一台">◀</button><button id="play" class="play" aria-label="播放或暂停">▶</button><button id="next" aria-label="下一台">▶</button></nav><div class="volume"><span>🔉</span><input id="volume" type="range" min="0" max="21" aria-label="音量"><span>🔊</span></div><div class="list-title"><h2>电台列表</h2><span id="count" class="count"></span></div><section id="stations" class="station-list"></section></main>
<script>
const q=s=>document.querySelector(s),textureStyles={none:['none','auto'],dots:['radial-gradient(#ffffff24 1px,transparent 1px)','18px 18px'],grid:['linear-gradient(#ffffff16 1px,transparent 1px),linear-gradient(90deg,#ffffff16 1px,transparent 1px)','24px 24px'],diagonal:['repeating-linear-gradient(135deg,#ffffff0d 0 2px,transparent 2px 12px)','auto'],cloud:['radial-gradient(circle at 12px 14px,transparent 9px,#ffffff1f 10px 11px,transparent 12px),radial-gradient(circle at 28px 14px,transparent 9px,#ffffff1f 10px 11px,transparent 12px)','40px 28px'],lattice:['linear-gradient(45deg,#ffffff14 12.5%,transparent 12.5% 37.5%,#ffffff14 37.5% 62.5%,transparent 62.5% 87.5%,#ffffff14 87.5%)','32px 32px'],waves:['radial-gradient(ellipse at 50% 100%,transparent 11px,#ffffff1c 12px 13px,transparent 14px)','34px 18px'],bamboo:['repeating-linear-gradient(90deg,transparent 0 30px,#ffffff16 31px 33px,transparent 34px 62px),repeating-linear-gradient(0deg,transparent 0 54px,#ffffff0d 55px 57px,transparent 58px 86px)','64px 88px'],ricepaper:['linear-gradient(25deg,#ffffff0a 1px,transparent 1px),linear-gradient(115deg,#ffffff08 1px,transparent 1px)','37px 53px,41px 47px'],porcelain:['radial-gradient(circle at 0 0,transparent 15px,#ffffff20 16px 17px,transparent 18px),radial-gradient(circle at 100% 100%,transparent 15px,#ffffff20 16px 17px,transparent 18px)','40px 40px']};
let stations=[],favorites=[],selected=-1,playerState='stopped',playlistRevision=0,refreshBusy=false,volumeTimer;
async function api(url,options){const r=await fetch(url,options);const t=await r.text();let d={};try{d=t?JSON.parse(t):{}}catch(_){d={error:t||'请求失败'}}if(!r.ok)throw Error(d.error||'请求失败');return d}
function safeLogo(name){return typeof name==='string'&&/^[A-Za-z0-9._-]+$/.test(name)?'/logos/'+encodeURIComponent(name):''}
function setImage(image,fallback,station){const name=(station&&station.name||'R').trim().slice(0,1).toUpperCase();fallback.textContent=name||'R';const url=safeLogo(station&&station.logo);if(!url){image.style.display='none';fallback.style.display='grid';return}image.style.display='block';fallback.style.display='none';image.onerror=()=>{image.style.display='none';fallback.style.display='grid'};image.src=url}
function updateRows(){document.querySelectorAll('[data-station-id]').forEach(row=>row.classList.toggle('active',Number(row.dataset.stationId)===selected))}
function renderNow(){const station=stations.find(s=>s.id===selected)||{name:'网络收音机',logo:''};q('#stationName').textContent=station.name;q('#play').textContent=playerState==='playing'?'Ⅱ':'▶';q('#state').textContent=({playing:'正在播放',buffering_or_reconnecting:'正在缓冲',stopped:'已暂停'})[playerState]||'正在恢复连接';q('#progress').classList.toggle('busy',playerState!=='playing'&&playerState!=='stopped');setImage(q('#cover'),q('#coverFallback'),station);updateRows()}
function stationButton(station,inFavorites=false){const row=document.createElement('button'),img=document.createElement('img'),fallback=document.createElement('span'),text=document.createElement('span'),name=document.createElement('b');row.className='station';row.dataset.stationId=station.id;row.addEventListener('click',()=>selectStation(station.id,inFavorites));img.loading='lazy';img.decoding='async';fallback.className='fallback';name.textContent=station.name;text.append(name);if(station.id===selected){const hint=document.createElement('small');hint.textContent='当前电台';text.append(hint)}setImage(img,fallback,station);row.append(img,fallback,text);return row}
function renderStations(){const host=q('#stations');host.replaceChildren();q('#count').textContent=stations.length+' 个电台';if(!stations.length){const e=document.createElement('div');e.className='notice';e.textContent='暂无电台，请到管理页面添加';host.append(e);return}const favoriteHeading=document.createElement('div');favoriteHeading.className='station-group';favoriteHeading.textContent='收藏（'+favorites.length+'）';host.append(favoriteHeading);favorites.map(id=>stations.find(s=>s.id===id)).filter(Boolean).forEach(station=>host.append(stationButton(station,true)));let group='';stations.forEach(station=>{if(station.group!==group){group=station.group;const heading=document.createElement('div');heading.className='station-group';heading.textContent=group;host.append(heading)}host.append(stationButton(station))});updateRows()}
function applyPlayer(data){playerState=data.state||playerState;if(Number.isInteger(data.selected_station))selected=data.selected_station;if(Number.isInteger(data.volume))q('#volume').value=data.volume;renderNow()}
async function loadStations(){const data=await api('/api/user/stations');stations=Array.isArray(data.stations)?data.stations:[];favorites=Array.isArray(data.favorites)?data.favorites:[];selected=data.selected;playlistRevision=data.revision||0;renderStations();renderNow()}
async function command(url){try{applyPlayer(await api(url,{method:'POST'}))}catch(e){alert(e.message)}}
function selectStation(id,inFavorites=false){selected=id;playerState='buffering_or_reconnecting';renderNow();command('/api/user/stations/select?id='+encodeURIComponent(id)+(inFavorites?'&context=favorites':''))}
async function refresh(){if(refreshBusy)return;refreshBusy=true;try{const data=await api('/api/user/player/status');applyPlayer(data);if(data.playlist_revision!==playlistRevision)await loadStations()}catch(e){q('#state').textContent='设备连接失败'}finally{refreshBusy=false;setTimeout(refresh,document.hidden?30000:5000)}}
q('#play').addEventListener('click',()=>command(playerState==='playing'?'/api/user/player/stop':'/api/user/player/play'));q('#previous').addEventListener('click',()=>command('/api/user/player/previous'));q('#next').addEventListener('click',()=>command('/api/user/player/next'));q('#volume').addEventListener('input',e=>{clearTimeout(volumeTimer);volumeTimer=setTimeout(()=>command('/api/user/player/volume?value='+encodeURIComponent(e.target.value)),180)});
document.addEventListener('visibilitychange',()=>{if(!document.hidden)refresh()});refresh();api('/api/user/theme').then(theme=>{document.documentElement.style.setProperty('--bg',theme.background);document.documentElement.style.setProperty('--accent',theme.accent);document.body.style.backgroundColor=theme.background;q('meta[name="theme-color"]').content=theme.background;const t=textureStyles[theme.texture]||textureStyles.none;q('.app').style.backgroundImage=t[0];q('.app').style.backgroundSize=t[1]}).catch(()=>{});
</script></body></html>
)HTML";

constexpr char kAdminHtmlV302[] PROGMEM =
R"HTML(<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>网络收音机 4.8.2 管理</title><style>
:root{color-scheme:dark}body{max-width:880px;margin:24px auto;padding:0 16px;background:#101827;color:#e5e7eb;font:16px system-ui,-apple-system,"PingFang SC","Microsoft YaHei",sans-serif}section,pre,.station,.wifi-network{background:#172234;padding:14px;border-radius:10px;margin:14px 0}button,input,select{box-sizing:border-box;padding:9px;margin:4px;border:0;border-radius:6px}input,select{width:100%}button{background:#38bdf8;color:#062032;font-weight:700;cursor:pointer}button:disabled{opacity:.38;cursor:not-allowed}.warn{background:#fbbf24}.danger{background:#fb7185}.playlist-head,.station-group{display:flex;align-items:center;justify-content:space-between;gap:10px;flex-wrap:wrap}.playlist-head h2,.station-group strong{margin:0}.station-group{margin:24px 2px 8px;padding:10px 12px;border:1px solid #263b55;border-radius:9px;color:#67e8f9;font-size:18px;font-weight:800}.group-actions{display:flex;flex-wrap:wrap;gap:3px}.group-actions button{min-width:auto;margin:0;padding:7px 10px;font-size:13px}.station{content-visibility:auto;contain-intrinsic-size:170px}.station img,.station .fallback{display:inline-grid;width:48px;height:48px;object-fit:contain;object-position:center;background:#fff;border-radius:8px;vertical-align:middle;margin-right:10px}.station .fallback{place-items:center;background:#e89c27;color:#fff;font-weight:700}.station small{display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:#b7c6da}.actions{display:block}.station button{min-width:82px;padding:11px 17px}.wifi-network{display:flex;align-items:center;justify-content:space-between;gap:10px;padding:10px}.wifi-network b{overflow:hidden;text-overflow:ellipsis}.wifi-network button{width:auto;margin:0}.active{outline:2px solid #38bdf8}.state{font-size:1.1em;color:#67e8f9;margin-bottom:24px}.transport{display:flex;align-items:center;justify-content:center;gap:clamp(28px,8vw,72px);margin:18px 0 28px}.transport button{display:grid;place-items:center;margin:0}.skip{width:76px;height:64px;border-radius:18px;font-size:25px;background:#263449;color:#dce6f5}.play{width:92px;height:92px;border-radius:50%;font-size:36px;background:#f8fafc;color:#172234;box-shadow:0 10px 28px #0005}.volume-head{display:flex;justify-content:space-between;align-items:center;margin:0 6px 8px;color:#cbd5e1}.volume-head b{color:#fff;font-size:1.15em}.volume{width:calc(100% - 10px);accent-color:#38bdf8}.theme-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}.theme-grid label{display:grid;gap:6px}.theme-grid input,.theme-grid select{margin:0}.theme-grid input[type=color]{height:54px;padding:4px;border:1px solid #ffffff26;border-radius:10px;background:#fff;color-scheme:light;cursor:pointer}.theme-grid input[type=color]::-webkit-color-swatch-wrapper{padding:0}.theme-grid input[type=color]::-webkit-color-swatch{border:0;border-radius:6px}.theme-grid input[type=color]::-moz-color-swatch{border:0;border-radius:6px}pre{overflow:auto;white-space:pre-wrap}a{color:#67e8f9}@media(max-width:560px){.theme-grid{grid-template-columns:1fr}.playlist-head{align-items:flex-start}.station{overflow-x:auto;white-space:nowrap;contain-intrinsic-size:190px}.station small{white-space:normal}.station button{min-width:auto;padding:9px 11px;margin:3px 2px}.group-actions{width:100%}.group-actions button{flex:1}}</style></head>
<body><h1>ESP32-S3 网络收音机</h1><p>版本号：)HTML"
NETWORK_RADIO_VERSION
R"HTML(　编译时间：)HTML"
__DATE__ " " __TIME__
R"HTML(　<a href="/">返回播放器</a></p>
<section class="player"><h2>正在播放</h2><div id="now" class="state">读取中…</div><div class="transport"><button id="previous" class="skip" aria-label="上一台">◀◀</button><button id="play" class="play" aria-label="播放或暂停">▶</button><button id="next" class="skip" aria-label="下一台">▶▶</button></div><div class="volume-head"><span>音量</span><b><span id="volumeText">--</span>/21</b></div><input id="volume" class="volume" type="range" min="0" max="21"></section>
<section><h2>用户页面外观</h2><div class="theme-grid"><label>页面颜色<input id="background" type="color" value="#656b6a"></label><label>强调颜色<input id="accent" type="color" value="#f2a51a"></label><label>纹理效果<select id="texture"><option value="none">无纹理</option><option value="dots">圆点</option><option value="grid">网格</option><option value="diagonal">斜纹</option><option value="cloud">祥云</option><option value="lattice">回纹窗格</option><option value="waves">水波</option><option value="bamboo">竹影</option><option value="ricepaper">宣纸</option><option value="porcelain">青花</option></select></label></div><button id="saveTheme">保存页面外观</button></section>
<section><h2>RGB 播放灯效</h2><p>仅在正常播放时生效；缓冲、错误和 OTA 状态灯优先显示。</p><div class="theme-grid"><label>灯效<select id="ledEffect"><option value="off">关闭</option><option value="rainbow">彩虹循环</option><option value="color_breathe">呼吸变色</option><option value="aurora">极光</option><option value="flame">火焰</option><option value="heartbeat">心跳</option><option value="meteor">流星</option><option value="pulse">脉冲</option><option value="random_fade">随机柔变</option><option value="music">音乐律动</option><option value="signal">状态渐变（Wi-Fi 信号）</option><option value="fixed_breathe">固定色呼吸</option><option value="temperature">色温变化</option><option value="starlight">闪烁星光</option></select></label><label>固定呼吸颜色<input id="ledFixedColor" type="color" value="#0080ff"></label></div><button id="saveLedEffect">保存 RGB 灯效</button></section>
<section><div class="playlist-head"><h2>播放列表</h2><button id="resetGroups" class="warn">恢复默认分组顺序</button></div><div id="stations">加载中…</div><h3 id="formTitle">新增电台</h3><input id="editId" type="hidden"><input id="stationName" placeholder="电台名称"><input id="stationUrl" placeholder="http(s):// 音频流地址"><button id="saveStation">保存</button><button id="cancelEdit" class="warn">取消编辑</button></section>
<section><h2>Wi-Fi</h2><p>最多保存 5 个网络；启动时会选择信号最强且可连接的已保存网络。</p><div id="savedWifi">读取已保存网络…</div><button id="scanWifi">扫描网络</button><select id="ssid"><option value="">选择 Wi-Fi</option></select><input id="wifiPassword" type="password" placeholder="Wi-Fi 密码（更新同名网络时请重新填写）"><button id="saveWifi">保存网络并重启连接</button><button id="forgetWifi" class="warn">清除全部 Wi-Fi 设置</button></section>
<section><h2>串口日志</h2><p>开关会立即生效并保存；关闭只停止串口输出，诊断日志仍会保留。</p><div id="serialLogs" style="display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px"><label style="display:flex;align-items:center;gap:8px"><input id="logSystem" type="checkbox" style="width:auto">系统 / 存储</label><label style="display:flex;align-items:center;gap:8px"><input id="logWifi" type="checkbox" style="width:auto">Wi-Fi</label><label style="display:flex;align-items:center;gap:8px"><input id="logAudio" type="checkbox" style="width:auto">音频 / 播放恢复</label><label style="display:flex;align-items:center;gap:8px"><input id="logTouch" type="checkbox" style="width:auto">触摸按键</label></div><small id="serialLogState">正在读取…</small></section>
<section><h2>维护与安全</h2><button id="chooseFirmware">选择固件并升级</button><input id="firmware" type="file" accept=".bin" hidden><button id="chooseResources" class="warn">选择资源镜像并升级</button><input id="resources" type="file" accept=".bin" hidden><p><small>资源升级请选择构建目录中的 <code>littlefs.bin</code>。它会更新台标、开机提示音等 LittleFS 文件，不会清除 Wi-Fi、电台或管理密码。</small></p><button id="downloadLog">下载诊断日志</button><input id="adminPassword" type="password" placeholder="设置管理密码（8–63 位，用户名 admin）"><button id="savePassword">保存管理密码</button><button id="factoryReset" class="danger">恢复出厂设置</button><p>配网热点密码独立：<code>radio-setup</code></p></section><pre id="status">读取中…</pre>
<script>
const q=s=>document.querySelector(s),enc=o=>new URLSearchParams(o);let stations=[],groups=[],favorites=[],selected=-1,playerState='stopped',playlistRevision=0,pollBusy=false,volumeTimer,pollCount=0;
async function api(url,options){const r=await fetch(url,options);const t=await r.text();let d={};try{d=t?JSON.parse(t):{}}catch(_){d={error:t||'请求失败'}}if(!r.ok)throw Error(d.error||'请求失败');return d}
function safeLogo(name){return typeof name==='string'&&/^[A-Za-z0-9._-]+$/.test(name)?'/logos/'+encodeURIComponent(name):''}
function button(label,handler,style,disabled=false){const b=document.createElement('button');b.textContent=label;if(style)b.className=style;b.disabled=disabled;b.addEventListener('click',handler);return b}
function stationImage(station){const image=document.createElement('img'),fallback=document.createElement('span'),url=safeLogo(station.logo);image.loading='lazy';image.decoding='async';fallback.className='fallback';fallback.textContent=(station.name||'R').trim().slice(0,1).toUpperCase()||'R';if(!url){image.style.display='none'}else{fallback.style.display='none';image.onerror=()=>{image.style.display='none';fallback.style.display='grid'};image.src=url}return [image,fallback]}
function stationRow(station,inFavorites=false,favoritePosition=-1){const row=document.createElement('article'),title=document.createElement('b'),url=document.createElement('small'),actions=document.createElement('div'),[image,fallback]=stationImage(station),isFavorite=favorites.includes(station.id);row.className='station'+(station.id===selected?' active':'');row.dataset.stationId=String(station.id);title.textContent=station.name;url.textContent=station.url;actions.className='actions';if(inFavorites){actions.append(button('播放此台',()=>post('/api/stations/select?id='+station.id+'&context=favorites')),button('最前',()=>postFavorite('/api/favorites/move?id='+station.id+'&direction=first'),'',favoritePosition===0),button('↑',()=>postFavorite('/api/favorites/move?id='+station.id+'&direction=up'),'',favoritePosition===0),button('↓',()=>postFavorite('/api/favorites/move?id='+station.id+'&direction=down'),'',favoritePosition===favorites.length-1),button('最后',()=>postFavorite('/api/favorites/move?id='+station.id+'&direction=last'),'',favoritePosition===favorites.length-1),button('取消收藏',()=>postFavorite('/api/favorites?id='+station.id+'&action=remove'),'warn'))}else{actions.append(button('播放此台',()=>post('/api/stations/select?id='+station.id)),button(isFavorite?'取消收藏':'加入收藏',()=>postFavorite('/api/favorites?id='+station.id+'&action='+(isFavorite?'remove':'add')),isFavorite?'warn':''),button('编辑',()=>editStation(station.id)),button('组内最前',()=>post('/api/stations/move?id='+station.id+'&direction=first')),button('↑',()=>post('/api/stations/move?id='+station.id+'&direction=up')),button('↓',()=>post('/api/stations/move?id='+station.id+'&direction=down')),button('组内最后',()=>post('/api/stations/move?id='+station.id+'&direction=last')),button('删除',()=>removeStation(station.id),'warn'))}row.append(image,fallback,title,url,actions);return row}
function renderStations(){const host=q('#stations');host.replaceChildren(),favoriteMeta=groups.find(g=>g.id===255)||{id:255,name:'收藏',count:favorites.length},favoriteHeading=document.createElement('div'),favoriteLabel=document.createElement('strong');favoriteHeading.className='station-group';favoriteLabel.textContent=favoriteMeta.name+'（'+favorites.length+'）';favoriteHeading.append(favoriteLabel);host.append(favoriteHeading);favorites.map(id=>stations.find(s=>s.id===id)).filter(Boolean).forEach((station,index)=>host.append(stationRow(station,true,index)));const visible=(groups.length?groups:Array.from(new Map(stations.map(s=>[s.group_id,{id:s.group_id,name:s.group,count:stations.filter(x=>x.group_id===s.group_id).length}])).values())).filter(g=>g.id!==255),positions=new Map(visible.map((g,i)=>[g.id,i]));let groupId=null;stations.forEach(station=>{if(station.group_id!==groupId){groupId=station.group_id;const meta=visible.find(g=>g.id===groupId)||{id:groupId,name:station.group,count:0},position=positions.get(groupId)||0,heading=document.createElement('div'),label=document.createElement('strong'),controls=document.createElement('div');heading.className='station-group';label.textContent=meta.name+'（'+meta.count+'）';controls.className='group-actions';controls.append(button('最前',()=>postGroup('/api/station-groups/move?id='+meta.id+'&direction=first'),'',position===0),button('上移',()=>postGroup('/api/station-groups/move?id='+meta.id+'&direction=up'),'',position===0),button('下移',()=>postGroup('/api/station-groups/move?id='+meta.id+'&direction=down'),'',position===visible.length-1),button('最后',()=>postGroup('/api/station-groups/move?id='+meta.id+'&direction=last'),'',position===visible.length-1));heading.append(label,controls);host.append(heading)}host.append(stationRow(station))});if(!stations.length){const p=document.createElement('p');p.textContent='暂无电台';host.append(p)}}
function applyPlaylist(data){if(!Array.isArray(data.stations))return;stations=data.stations;groups=Array.isArray(data.groups)?data.groups:[];favorites=Array.isArray(data.favorites)?data.favorites:[];selected=data.selected;playlistRevision=data.revision||playlistRevision;renderStations()}
function applyPlayer(data){playerState=data.state||playerState;if(Number.isInteger(data.selected_station))selected=data.selected_station;if(Number.isInteger(data.volume)){q('#volume').value=data.volume;q('#volumeText').textContent=data.volume}const current=stations.find(s=>s.id===selected);q('#now').textContent=(data.state||'stopped')+' · '+(current?current.name:'网络收音机')+(data.message?' · '+data.message:'');q('#play').textContent=playerState==='playing'?'Ⅱ':'▶';document.querySelectorAll('.station').forEach(row=>row.classList.toggle('active',Number(row.dataset.stationId)===selected))}
async function loadPlaylist(){applyPlaylist(await api('/api/stations'))}
async function refreshPlayer(reloadPlaylist=true){const data=await api('/api/player/status');applyPlayer(data);if(reloadPlaylist&&data.playlist_revision!==playlistRevision)await loadPlaylist()}
async function refreshStatus(){const data=await api('/api/status');q('#status').textContent=JSON.stringify(data,null,2)}
async function poll(){if(pollBusy)return;pollBusy=true;try{await refreshPlayer();if(++pollCount%6===0)await refreshStatus()}catch(e){q('#status').textContent='错误：'+e.message}finally{pollBusy=false;setTimeout(poll,document.hidden?30000:5000)}}
async function post(url){try{const data=await api(url,{method:'POST'});applyPlaylist(data);applyPlayer(data)}catch(e){alert(e.message)}}
async function postGroup(url){try{await api(url,{method:'POST'});await loadPlaylist()}catch(e){try{await loadPlaylist()}catch(_){}alert(e.message)}}
async function postFavorite(url){try{applyPlaylist(await api(url,{method:'POST'}))}catch(e){alert(e.message)}}
function editStation(id){const s=stations.find(x=>x.id===id);if(!s)return;q('#editId').value=id;q('#stationName').value=s.name;q('#stationUrl').value=s.url;q('#formTitle').textContent='编辑电台'}
function clearForm(){q('#editId').value='';q('#stationName').value='';q('#stationUrl').value='';q('#formTitle').textContent='新增电台'}
async function saveStation(){const id=q('#editId').value,body=enc({name:q('#stationName').value,url:q('#stationUrl').value});try{const data=await api(id===''?'/api/stations':'/api/stations/update?id='+encodeURIComponent(id),{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});applyPlaylist(data);clearForm()}catch(e){alert(e.message)}}
function removeStation(id){if(confirm('删除该电台？'))post('/api/stations/delete?id='+id)}
async function scanWifi(){try{let data;for(let attempt=0;attempt<25;attempt++){data=await api('/api/wifi/scan');if(!data.scanning)break;await new Promise(resolve=>setTimeout(resolve,350))}if(!data||data.scanning)throw Error('Wi-Fi 扫描超时');const select=q('#ssid');select.replaceChildren();const empty=document.createElement('option');empty.value='';empty.textContent='选择 Wi-Fi';select.append(empty);(data.networks||[]).forEach(network=>{const option=document.createElement('option');option.value=network.ssid;option.textContent=network.ssid+' ('+network.rssi+' dBm)';select.append(option)})}catch(e){alert(e.message)}}
function renderSavedWifi(data){const host=q('#savedWifi');host.replaceChildren();const networks=data.saved||[];networks.forEach(network=>{const row=document.createElement('div'),name=document.createElement('b'),note=document.createElement('small'),remove=document.createElement('button');row.className='wifi-network';name.textContent=network.ssid;note.textContent=network.ssid===data.connected_ssid?'当前已连接':'启动时自动选择';remove.textContent='删除';remove.className='warn';remove.addEventListener('click',()=>deleteWifi(network.ssid));const text=document.createElement('div');text.append(name,document.createElement('br'),note);row.append(text,remove);host.append(row)});if(!networks.length){const p=document.createElement('p');p.textContent='尚未保存 Wi-Fi 网络。';host.append(p)}}
async function loadSavedWifi(){try{renderSavedWifi(await api('/api/wifi'))}catch(e){q('#savedWifi').textContent='无法读取已保存网络：'+e.message}}
async function saveWifi(){try{await api('/api/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc({ssid:q('#ssid').value,password:q('#wifiPassword').value})});q('#status').textContent='Wi-Fi 已保存，设备正在重启并选择可用网络…'}catch(e){alert(e.message)}}
async function deleteWifi(ssid){if(!confirm('删除已保存的 Wi-Fi “'+ssid+'”？'))return;try{await api('/api/wifi/delete?ssid='+encodeURIComponent(ssid),{method:'POST'});q('#status').textContent='Wi-Fi 已删除，设备正在重启…'}catch(e){alert(e.message)}}
async function saveTheme(){try{const data=await api('/api/ui-theme',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc({background:q('#background').value,accent:q('#accent').value,texture:q('#texture').value})});q('#background').value=data.background;q('#accent').value=data.accent;q('#texture').value=data.texture;alert('页面外观已保存')}catch(e){alert(e.message)}}
function applyLedSettings(data){q('#ledEffect').value=data.effect;q('#ledFixedColor').value=data.fixed_color}
async function saveLedSettings(){try{const data=await api('/api/led-effect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc({effect:q('#ledEffect').value,fixed_color:q('#ledFixedColor').value})});applyLedSettings(data);alert('RGB 灯效已保存并立即生效')}catch(e){alert(e.message)}}
function applySerialLogs(data){q('#logSystem').checked=!!data.system;q('#logWifi').checked=!!data.wifi;q('#logAudio').checked=!!data.audio;q('#logTouch').checked=!!data.touch;q('#serialLogState').textContent='已保存'}
async function loadSerialLogs(){try{applySerialLogs(await api('/api/serial-logs'))}catch(e){q('#serialLogState').textContent='读取失败：'+e.message}}
async function saveSerialLogs(){q('#serialLogState').textContent='正在保存…';try{const data=await api('/api/serial-logs',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc({system:q('#logSystem').checked,wifi:q('#logWifi').checked,audio:q('#logAudio').checked,touch:q('#logTouch').checked})});applySerialLogs(data)}catch(e){q('#serialLogState').textContent='保存失败：'+e.message;alert(e.message)}}
async function savePassword(){try{await api('/api/security/password',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc({password:q('#adminPassword').value})});alert('管理密码已保存；请刷新页面并用 admin 登录。')}catch(e){alert(e.message)}}
async function uploadFirmware(file){if(!file||!confirm('上传后设备会重启，继续？'))return;const form=new FormData;form.append('firmware',file);try{await api('/api/ota',{method:'POST',body:form});q('#status').textContent='升级完成，设备正在重启…'}catch(e){alert(e.message)}}
async function uploadResources(file){if(!file||!confirm('资源将被替换，上传后设备会重启；Wi-Fi 与电台设置会保留。继续？'))return;const form=new FormData;form.append('resources',file);try{await api('/api/ota/resources',{method:'POST',body:form});q('#status').textContent='资源升级完成，设备正在重启…'}catch(e){alert(e.message)}}
q('#previous').addEventListener('click',()=>post('/api/player/previous'));q('#play').addEventListener('click',()=>post(playerState==='playing'?'/api/player/stop':'/api/player/play'));q('#next').addEventListener('click',()=>post('/api/player/next'));q('#volume').addEventListener('input',e=>{q('#volumeText').textContent=e.target.value;clearTimeout(volumeTimer);volumeTimer=setTimeout(()=>post('/api/player/volume?value='+encodeURIComponent(e.target.value)),180)});q('#saveStation').addEventListener('click',saveStation);q('#cancelEdit').addEventListener('click',clearForm);q('#resetGroups').addEventListener('click',()=>{if(confirm('恢复默认分组顺序？组内电台顺序不会改变。'))postGroup('/api/station-groups/reset')});q('#scanWifi').addEventListener('click',scanWifi);q('#saveWifi').addEventListener('click',saveWifi);q('#forgetWifi').addEventListener('click',()=>{if(confirm('清除全部已保存的 Wi-Fi？'))post('/api/wifi/forget')});q('#saveTheme').addEventListener('click',saveTheme);q('#saveLedEffect').addEventListener('click',saveLedSettings);['#logSystem','#logWifi','#logAudio','#logTouch'].forEach(id=>q(id).addEventListener('change',saveSerialLogs));q('#savePassword').addEventListener('click',savePassword);q('#chooseFirmware').addEventListener('click',()=>q('#firmware').click());q('#firmware').addEventListener('change',e=>uploadFirmware(e.target.files[0]));q('#chooseResources').addEventListener('click',()=>q('#resources').click());q('#resources').addEventListener('change',e=>uploadResources(e.target.files[0]));q('#downloadLog').addEventListener('click',()=>location='/api/diagnostics/download');q('#factoryReset').addEventListener('click',()=>{if(confirm('这将清除 Wi-Fi、电台、音量、页面外观、RGB 灯效和管理密码，确定？'))post('/api/factory-reset')});document.addEventListener('visibilitychange',()=>{if(!document.hidden)poll()});(async()=>{try{await refreshPlayer(false);await loadPlaylist();applyPlayer({});await Promise.all([refreshStatus(),loadSavedWifi(),loadSerialLogs(),api('/api/ui-theme').then(t=>{q('#background').value=t.background;q('#accent').value=t.accent;q('#texture').value=t.texture}),api('/api/led-effect').then(applyLedSettings)]);poll()}catch(e){q('#status').textContent='错误：'+e.message}})();
</script></body></html>
)HTML";

void configureWebServerV8() {
  server.serveStatic("/logos/", LittleFS, "/logos/", "max-age=86400");
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html; charset=utf-8", kUserHtmlV301); });
  server.on("/admin", HTTP_GET, [] { if (requireAdmin()) server.send_P(200, "text/html; charset=utf-8", kAdminHtmlV302); });
  server.on("/api/user/stations", HTTP_GET, [] { sendPlaylistJson(false); });
  server.on("/api/user/theme", HTTP_GET, [] { sendJson(uiThemeJson()); });
  server.on("/api/user/stations/select", HTTP_POST, handleUserSelectStation);
  server.on("/api/user/player/status", HTTP_GET, handleUserPlayerStatus);
  server.on("/api/user/player/play", HTTP_POST, handleUserPlay);
  server.on("/api/user/player/stop", HTTP_POST, handleUserStop);
  server.on("/api/user/player/volume", HTTP_POST, handleUserVolume);
  server.on("/api/user/player/previous", HTTP_POST, handleUserPrevious);
  server.on("/api/user/player/next", HTTP_POST, handleUserNext);
  server.on("/api/status", HTTP_GET, handleStatusV8);
  server.on("/api/stations", HTTP_GET, [] {
    if (requireAdmin()) sendPlaylistJson(true);
  });
  server.on("/api/stations", HTTP_POST, [] { if (requireAdmin()) handleAddStation(); });
  server.on("/api/stations/update", HTTP_POST, [] { if (requireAdmin()) handleUpdateStation(); });
  server.on("/api/stations/delete", HTTP_POST, [] { if (requireAdmin()) handleDeleteStation(); });
  server.on("/api/stations/select", HTTP_POST, [] { if (requireAdmin()) handleSelectStation(); });
  server.on("/api/stations/move", HTTP_POST, [] { if (requireAdmin()) handleMoveStationV11(); });
  server.on("/api/favorites", HTTP_POST, [] {
    if (requireAdmin()) handleSetFavoriteStation();
  });
  server.on("/api/favorites/move", HTTP_POST, [] {
    if (requireAdmin()) handleMoveFavoriteStation();
  });
  server.on("/api/station-groups/move", HTTP_POST, [] {
    if (requireAdmin()) handleMoveStationGroup();
  });
  server.on("/api/station-groups/reset", HTTP_POST, [] {
    if (requireAdmin()) handleResetStationGroups();
  });
  server.on("/api/wifi/scan", HTTP_GET, [] { if (requireAdmin()) handleWifiScan(); });
  server.on("/api/wifi", HTTP_GET, [] { if (requireAdmin()) sendJson(savedWifiNetworksJson()); });
  server.on("/api/wifi", HTTP_POST, handleSaveWifi);
  server.on("/api/wifi/delete", HTTP_POST, handleDeleteWifi);
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
  server.on("/api/serial-logs", HTTP_GET, [] {
    if (requireAdmin()) sendJson(serialLogSettingsJson());
  });
  server.on("/api/serial-logs", HTTP_POST, handleSaveSerialLogSettings);
  server.on("/api/ui-theme", HTTP_GET, [] { if (requireAdmin()) sendJson(uiThemeJson()); });
  server.on("/api/ui-theme", HTTP_POST, handleSaveUiTheme);
  server.on("/api/led-effect", HTTP_GET, [] {
    if (requireAdmin()) sendJson(ledSettingsJson());
  });
  server.on("/api/led-effect", HTTP_POST, handleSaveLedSettings);
  server.on("/api/factory-reset", HTTP_POST, handleFactoryReset);
  server.on("/api/ota", HTTP_POST, handleOtaResult, handleOtaUpload);
  server.on("/api/ota/resources", HTTP_POST, handleOtaResult, handleResourceOtaUpload);
  server.onNotFound([] { server.sendHeader("Location", "/"); server.send(302, "text/plain", "Redirecting"); });
  server.begin();
}

}  // namespace

#ifndef NETWORK_RADIO_V8_NO_ENTRYPOINT
void setup() {
  Serial.begin(config::kSerialBaud); delay(300);
  loadSerialLogSettings();
  rgbLedWrite(kStatusLedPin, 0, 0, 0);
  snprintf(accessPointSsid, sizeof(accessPointSsid), "%s%04X", config::kAccessPointPrefix, setupAccessPointId());
  loadSecurity();
  if (!allocateStationStore()) {
    rgbLedWrite(kStatusLedPin, kStatusLedBrightness, 0, 0);
    Serial.println("FATAL: Network Radio requires PSRAM for its station store.");
    while (true) delay(1000);
  }
  playlistStorageReady = LittleFS.begin(false);
  serialLogPrintf(kSerialLogSystemBit, "LittleFS: %s\n",
                  playlistStorageReady
                      ? "mounted"
                      : "mount failed; using NVS fallback");
  loadPlaylist();
  loadFavoritePlaybackContext();
  if (playlistFormatMigrationPending && persistPlaylist()) {
    serialLogPrintln(kSerialLogSystemBit,
                     "Playlist storage migrated to 16-bit station indexes.");
    playlistFormatMigrationPending = false;
  }
  migrateStationCatalog();
  importBuiltinStations();
  const bool expandedStationsAdded = importExpandedStationPack();
  enrichStationIcons();
  initialiseStationGroups(expandedStationsAdded);
  loadFavoriteStations();
  validateFavoritePlaybackContext();
  loadUiTheme();
  loadLedSettings();
  playerPreferences.begin("player", true); playerVolume = playerPreferences.getUChar("volume", playerVolume); playerPreferences.end();
  Audio::audio_info_callback = audioInfoV8;
  audio.settings.BUFFER_TRESHOLD_HLS = 32 * 1024;
  // Initialise I2S exactly once and reuse the same Audio instance for the
  // startup chime and the subsequent network stream.
  initialiseAudioOutput();
  playBootChime();
  audioOutputPeak = 0;
  const bool connected = connectSavedStation();
  if (!connected || config::kKeepSetupAccessPointAvailable) startAccessPoint();
  wasStationConnected = connected;
  onStationSelected = onPlaylistSelectionV8;
  configureWebServerV8();
  initialiseTouchButtons();
  addLog("boot", config::kFirmwareVersion);
  if (connected) startSelectedStationV8("boot playback");
}

void loop() {
  if (accessPointRunning) dnsServer.processNextRequest();
  server.handleClient();
  pollSerialCommands();
  pollTouchButtons();
  printTouchDebugIfDue();
  maintainNetworkAndPlayback();
  audio.loop();
  updateStatusLed();
}
#endif  // NETWORK_RADIO_V8_NO_ENTRYPOINT
