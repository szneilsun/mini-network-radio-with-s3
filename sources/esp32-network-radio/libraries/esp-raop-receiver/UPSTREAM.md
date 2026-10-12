# ESP RAOP Receiver

This directory contains the RAOP receiver component from
[`jptrsn/esp-airsync`](https://github.com/jptrsn/esp-airsync), pinned to:

- release: `v0.2.4`
- commit: `7601fe7cca49ac4141e72108bd34727aec0aa883`
- retrieved: 2026-10-12

Included files are the public API, RAOP/RTP implementation, buffering code,
and the upstream precompiled ALAC archive. The esp-airsync application,
provisioning, web UI, OTA, I2S output, and WLED components are not included.

The local integration also aborts RAOP SETUP cleanly when either PSRAM audio
buffer or the RTP context cannot be allocated, so the receiver does not fall
back to exhausting internal RAM and the radio can recover normally.

The component is licensed under GPLv3; see `LICENSE`. The ALAC decoder is
identified by upstream as Apache-2.0 licensed. See the repository root
`THIRD_PARTY_NOTICES.md` for source and attribution details.

