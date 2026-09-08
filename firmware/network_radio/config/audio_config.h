#pragma once

#include <Arduino.h>

namespace audio_config {

// Audio output is intentionally MAX98357A only. It accepts 16-bit Philips
// I2S and directly drives one BTL-connected 4-8 ohm speaker.
constexpr char kOutputDeviceName[] = "MAX98357A";
constexpr uint8_t kAmplifierSupplyVolts = 5;
constexpr bool kDefaultMixedMono = true;  // SD_MODE and GAIN remain unconnected.

// This matches the verified AAC radio stream.
constexpr uint32_t kSampleRateHz = 48000;
constexpr uint8_t kBitsPerSample = 16;
constexpr uint16_t kChannels = 2;

// Step 1 hardware test. 8% full-scale is deliberately conservative for a
// 5 V MAX98357A driving a 4 ohm speaker.
constexpr float kTestToneHz = 440.0F;
constexpr uint8_t kTestTonePercent = 8;
constexpr uint16_t kToneFramesPerWrite = 256;
constexpr uint16_t kFadeInMilliseconds = 150;

static_assert(kSampleRateHz == 48000);
static_assert(kBitsPerSample == 16);
static_assert(kChannels == 2);

}  // namespace audio_config
