#include <Arduino.h>
#include <ESP_I2S.h>

#include "config/audio_config.h"
#include "config/board_config.h"
#include "web_server.h"

namespace {

I2SClass i2s(I2S_NUM_0);
int16_t pcmBuffer[audio_config::kToneFramesPerWrite * audio_config::kChannels];

float phase = 0.0F;
uint32_t framesProduced = 0;
volatile bool testToneEnabled = true;

constexpr float kTwoPi = 6.28318530718F;
constexpr float kPhaseStep =
    kTwoPi * audio_config::kTestToneHz / audio_config::kSampleRateHz;
constexpr int16_t kPeak =
    static_cast<int16_t>(32767L * audio_config::kTestTonePercent / 100L);
constexpr uint32_t kFadeInFrames =
    audio_config::kSampleRateHz * audio_config::kFadeInMilliseconds / 1000U;

float fadeGain() {
  if (framesProduced >= kFadeInFrames) {
    return 1.0F;
  }
  return static_cast<float>(framesProduced) /
         static_cast<float>(kFadeInFrames);
}

void fillToneBuffer(bool enabled) {
  for (uint16_t frame = 0; frame < audio_config::kToneFramesPerWrite; ++frame) {
    const int16_t sample = enabled
                               ? static_cast<int16_t>(sinf(phase) *
                                                      static_cast<float>(kPeak) *
                                                      fadeGain())
                               : 0;
    pcmBuffer[frame * 2] = sample;
    pcmBuffer[frame * 2 + 1] = sample;

    phase += kPhaseStep;
    if (phase >= kTwoPi) {
      phase -= kTwoPi;
    }
    ++framesProduced;
  }
}

[[noreturn]] void stopWithError(const char *message) {
  Serial.println(message);
  while (true) {
    delay(1000);
  }
}

void printBootReport() {
  Serial.println();
  Serial.println("=== Network Radio / MAX98357A hardware test ===");
  Serial.printf("Device: %s\n", board::kDeviceName);
  Serial.printf("I2S: BCLK=GPIO%d, LRCLK=GPIO%d, DIN=GPIO%d\n",
                board::kI2sBclkPin, board::kI2sWsPin,
                board::kI2sDataOutPin);
  Serial.printf("Audio: %lu Hz, %u-bit, %u channels, %.1f Hz test tone\n",
                audio_config::kSampleRateHz, audio_config::kBitsPerSample,
                audio_config::kChannels, audio_config::kTestToneHz);
  Serial.printf("Flash: %u bytes, PSRAM: %u bytes%s\n", ESP.getFlashChipSize(),
                ESP.getPsramSize(), psramFound() ? "" : " (PSRAM NOT FOUND)");
  Serial.printf("Output: %s, VDD=%uV; speaker only across OUT+/OUT-.\n",
                audio_config::kOutputDeviceName,
                audio_config::kAmplifierSupplyVolts);
}

}  // namespace

void setup() {
  Serial.begin(board::kSerialBaud);
  delay(300);
  printBootReport();

  i2s.setPins(board::kI2sBclkPin, board::kI2sWsPin,
              board::kI2sDataOutPin);
  if (!i2s.begin(I2S_MODE_STD, audio_config::kSampleRateHz,
                 I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                 I2S_STD_SLOT_BOTH)) {
    stopWithError("ERROR: I2S initialization failed.");
  }

  web::begin(&testToneEnabled);
  Serial.println("I2S ready. A low-volume 440 Hz tone will play continuously.");
}

void loop() {
  web::handleClient();
  fillToneBuffer(testToneEnabled);
  const size_t written = i2s.write(pcmBuffer, sizeof(pcmBuffer));
  if (written != sizeof(pcmBuffer)) {
    Serial.printf("WARN: I2S short write: %u/%u bytes\n",
                  static_cast<unsigned>(written),
                  static_cast<unsigned>(sizeof(pcmBuffer)));
  }
  web::handleClient();
}
