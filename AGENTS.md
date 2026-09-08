# Repository Guidelines

## Project Structure & Module Organization

`firmware/` contains versioned Arduino sketches. Each sketch lives in a directory whose name matches its `.ino` entry point, for example `firmware/network_radio_v3_0/network_radio_v3_0.ino`. Keep version-specific partition tables, generated headers, web assets, and README notes beside that sketch. The current standalone release is `network_radio_v3_0`; earlier directories document incremental features and compatibility history.

`tools/` holds the build, flash-argument, and flash-layout scripts. `hardware-web/` is a static hardware reference site; place shared styling and images in `hardware-web/assets/`. Root-level Markdown files contain board documentation. Generated firmware belongs under `build/` and should not be edited manually.

## Build, Test, and Development Commands

Install `arduino-cli`, the Espressif ESP32 core, and the libraries included by the target sketch. Then use:

```bash
./tools/build-firmware.sh
./tools/build-firmware.sh --upload --port /dev/cu.usbserial-XXXX
ESP32_SKETCH_DIR="$PWD/firmware/network_radio_v10_user_admin_ui" ./tools/build-firmware.sh
```

The first command compiles the default V3 sketch, exports binaries, and verifies the 16 MiB flash image. The second also flashes a connected ESP32-S3. The environment-variable form builds another version; that sketch must include `partitions.csv`.

Preview documentation pages with `python3 -m http.server 8000 -d hardware-web`, then open `http://localhost:8000`.

## Coding Style & Naming Conventions

Follow existing Arduino/C++ style: two-space indentation, opening braces on the same line, `camelCase` functions and variables, `PascalCase` types, and `kPascalCase` constants. Use uppercase snake case for compile-time macros. Prefer fixed-width integer types for hardware-facing values. Shell scripts must use Bash, quote expansions, and retain `set -euo pipefail`. Use lowercase kebab-case for web files and snake_case for firmware directories.

## Testing Guidelines

There is no unit-test framework or coverage target. A clean firmware build is the required automated check; it also runs `tools/verify-flash-layout.sh`. For firmware changes, record the target board, serial output, Wi-Fi/playback behavior, and OTA result tested. For web changes, check both mobile and desktop layouts and confirm linked assets load.

## Commit & Pull Request Guidelines

Git history is not included in this workspace snapshot. Use short, imperative commit subjects, optionally scoped, such as `firmware: retry HLS playback after Wi-Fi recovery`. Keep generated binaries out of commits unless they are intentional release artifacts. Pull requests should identify the affected firmware version, summarize behavior and compatibility impact, list build/hardware validation, link related issues, and include screenshots for visible web UI changes.

## Security & Configuration

Never commit Wi-Fi credentials, administrator passwords, private stream URLs, or device-specific secrets. Preserve partition offsets and NVS keys unless the change includes an explicit migration plan.
