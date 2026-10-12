# Third-Party Notices

## esp-airsync / ESP RAOP Receiver

- Project: [jptrsn/esp-airsync](https://github.com/jptrsn/esp-airsync)
- Version: `v0.2.4`
- Commit: `7601fe7cca49ac4141e72108bd34727aec0aa883`
- License: GNU General Public License v3.0
- Included: RAOP, RTP, buffering, metadata parsing, utility code, public API,
  private ALAC wrapper header, and the upstream ALAC static archive
- Excluded: upstream Wi-Fi provisioning, web, OTA, WLED, and I2S application
  components

The complete pinned upstream source is available from the project URL and tag.
A copy of GPLv3 is present at the repository root and beside the vendored
component.

The esp-airsync README credits protocol and buffering work derived from:

- [squeezelite-esp32](https://github.com/sle118/squeezelite-esp32), GPLv3 and
  MIT components
- [shairport-sync](https://github.com/mikebrady/shairport-sync), GPLv2+

## ALAC Decoder

The vendored `libalac.a` is distributed by esp-airsync and identified by that
project as the Apple Lossless Audio Codec decoder under Apache License 2.0.
The decoder source is available from
[macosforge/alac](https://github.com/macosforge/alac). A copy of Apache
License 2.0 is included at
`sources/esp32-network-radio/libraries/esp-raop-receiver/licenses/ALAC-APACHE-2.0.txt`.

## ESP32-audioI2S

- Project: [schreibfaul1/ESP32-audioI2S](https://github.com/schreibfaul1/ESP32-audioI2S)
- Use: network radio stream decoding and playback

The build uses the user's installed library and applies the project patch at
`tools/patches/esp32-audioi2s-hls-refresh.patch` to an isolated build copy.
