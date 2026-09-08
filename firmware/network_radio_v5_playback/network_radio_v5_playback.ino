/* Network Radio V5: V4 playlist management plus HLS/AAC playback. */

#include <Audio.h>

// Reuse V4's provisioning, playlist persistence and web UI. Its setup/loop
// are renamed because Audio owns the I2S peripheral in this version.
#ifndef NETWORK_RADIO_VERSION
#define NETWORK_RADIO_VERSION "0.5.0-playback"
#endif
#define setup v4_setup_unused
#define loop v4_loop_unused
#include "../network_radio_v4_playlist/network_radio_v4_playlist.ino"
#undef setup
#undef loop

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
                String(selectedStation) + ",\"input_buffer_bytes\":" +
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

#ifndef NETWORK_RADIO_NO_ENTRYPOINT
void setup() {
  Serial.begin(config::kSerialBaud);
  delay(300);
  snprintf(accessPointSsid, sizeof(accessPointSsid), "%s%04X",
           config::kAccessPointPrefix, setupAccessPointId());
  loadPlaylist();

  playerPreferences.begin("player", true);
  playerVolume = playerPreferences.getUChar("volume", playerVolume);
  playerPreferences.end();
  Audio::audio_info_callback = audioInfo;
  audio.settings.BUFFER_TRESHOLD_HLS = 32 * 1024;
  audio.setPinout(config::kI2sBclk, config::kI2sLrclk, config::kI2sDataOut);
  audio.setVolume(playerVolume);

  const bool connected = connectSavedStation();
  if (!connected || config::kKeepSetupAccessPointAvailable) startAccessPoint();
  onStationSelected = onPlaylistSelection;
  registerAdditionalRoutes = registerPlayerRoutes;
  configureWebServer();
  if (connected) startSelectedStation();
  Serial.printf("Network Radio %s ready; selected station: %s\n",
                "0.5.0-playback", stations[selectedStation].name);
}

void loop() {
  if (accessPointRunning) dnsServer.processNextRequest();
  server.handleClient();
  maintainStationConnection();
  audio.loop();
}
#endif  // NETWORK_RADIO_NO_ENTRYPOINT
