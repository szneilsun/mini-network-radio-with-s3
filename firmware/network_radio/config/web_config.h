#pragma once

#include <Arduino.h>

namespace web_config {

// Step 2 exposes a local, WPA2-protected setup hotspot. Wi-Fi client
// provisioning arrives in Step 3; do not add home-network credentials here.
constexpr char kAccessPointPrefix[] = "Radio-";
constexpr char kAccessPointPassword[] = "radio-setup";
constexpr uint8_t kAccessPointChannel = 6;
constexpr uint8_t kAccessPointMaxClients = 4;
constexpr uint16_t kHttpPort = 80;

static_assert(sizeof(kAccessPointPassword) - 1 >= 8,
              "WPA2 password must contain at least eight characters");

}  // namespace web_config
