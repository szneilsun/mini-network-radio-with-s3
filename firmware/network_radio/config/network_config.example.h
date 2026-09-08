#pragma once

// Reserved for Step 3. Do not put production Wi-Fi credentials in source.
// The final firmware will provision these values through its local web page
// and store them in NVS instead.
namespace network_config {
constexpr char kSetupApPrefix[] = "Radio-";
constexpr uint16_t kSetupApChannel = 6;
}
