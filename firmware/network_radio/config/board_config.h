#pragma once

#include <Arduino.h>

namespace board {

// Confirmed ESP32-S3-WROOM-1-N16R8 wiring. Keep audio on these low-conflict
// pins; GPIO19/20, GPIO43/44 and GPIO26-37 have board-level constraints.
constexpr char kDeviceName[] = "ESP32-S3 Network Radio";
constexpr char kFirmwareVersion[] = "0.2.0-step2-web";
constexpr uint32_t kSerialBaud = 115200;

constexpr gpio_num_t kI2sBclkPin = GPIO_NUM_4;
constexpr gpio_num_t kI2sWsPin = GPIO_NUM_5;
constexpr gpio_num_t kI2sDataOutPin = GPIO_NUM_6;

static_assert(kI2sBclkPin != kI2sWsPin);
static_assert(kI2sBclkPin != kI2sDataOutPin);
static_assert(kI2sWsPin != kI2sDataOutPin);

}  // namespace board
