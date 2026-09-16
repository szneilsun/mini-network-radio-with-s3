# Repository Guidelines

## Project Structure & Module Organization

`sources/esp32-network-radio/` contains the single maintained Arduino sketch, `esp32-network-radio.ino`, its partition table, and LittleFS assets under `data/logos/`. Project documentation belongs in the root `README.md` or `docs/`. Update the source directory in place for new versions; do not create versioned source directories.

`tools/` holds build and flash-layout scripts. `docs/` contains project and hardware documentation. `build/` is exclusively for local intermediate output. `release/` contains only verified, versioned ZIP packages; do not place extracted packages or device backups there.

## Build, Test, and Development Commands

Install `arduino-cli`, the Espressif ESP32 core, and the libraries included by the target sketch. Then use:

```bash
./tools/build-firmware.sh
./tools/build-firmware.sh --upload --port /dev/cu.usbserial-XXXX
```

The first command compiles the current sketch into `build/` and verifies the 16 MiB flash image. The second also flashes a connected ESP32-S3.

Preview documentation pages with `python3 -m http.server 8000 -d docs/hardware-web`, then open `http://localhost:8000`.

## Coding Style & Naming Conventions

Follow existing Arduino/C++ style: two-space indentation, opening braces on the same line, `camelCase` functions and variables, `PascalCase` types, and `kPascalCase` constants. Use uppercase snake case for compile-time macros. Prefer fixed-width integer types for hardware-facing values. Shell scripts must use Bash, quote expansions, and retain `set -euo pipefail`. Use lowercase kebab-case for web files and snake_case for firmware directories.

Whenever the firmware version changes, add a matching entry to `CHANGELOG.md`. Record user-visible features, fixes, compatibility changes, hardware requirements, and upgrade notes; keep unreleased work under `未发布`.

## Testing Guidelines

There is no unit-test framework or coverage target. A clean firmware build is the required automated check; it also runs `tools/verify-flash-layout.sh`. For firmware changes, record the target board, serial output, Wi-Fi/playback behavior, and OTA result tested. For web changes, check both mobile and desktop layouts and confirm linked assets load.

## Commit & Pull Request Guidelines

Git history is not included in this workspace snapshot. Use short, imperative commit subjects, optionally scoped, such as `firmware: retry HLS playback after Wi-Fi recovery`. Keep generated binaries out of commits unless they are intentional release artifacts. Pull requests should identify the affected firmware version, summarize behavior and compatibility impact, list build/hardware validation, link related issues, and include screenshots for visible web UI changes.

## Security & Configuration

Never commit Wi-Fi credentials, administrator passwords, private stream URLs, or device-specific secrets. Preserve partition offsets and NVS keys unless the change includes an explicit migration plan.
