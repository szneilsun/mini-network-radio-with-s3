#!/usr/bin/env bash
set -euo pipefail

package_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
sketch_dir="${package_root}/network_radio_v3_1_lcd"
build_dir="${package_root}/build"
output_dir="${package_root}/binaries"
arduino_cli=${ARDUINO_CLI:-arduino-cli}
fqbn="esp32:esp32:esp32s3:UploadSpeed=460800,USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,PSRAM=opi,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,ZigbeeMode=default"
project_name="network_radio_v3_1_lcd.ino"
partition_size=10289152

if ! command -v "$arduino_cli" >/dev/null 2>&1; then
  echo "arduino-cli was not found. Install it or set ARDUINO_CLI." >&2
  exit 127
fi

if ! "$arduino_cli" core list | awk '$1 == "esp32:esp32" && $2 == "3.3.11" { found=1 } END { exit !found }'; then
  echo "ESP32 Arduino core 3.3.11 is required." >&2
  echo "Install it with: arduino-cli core install esp32:esp32@3.3.11" >&2
  exit 69
fi

mkdir -p "$build_dir" "$output_dir"

"$arduino_cli" compile \
  --fqbn "$fqbn" \
  --libraries "${package_root}/libraries" \
  --build-path "$build_dir" \
  "$sketch_dir"

properties=$("$arduino_cli" compile --show-properties \
  --fqbn "$fqbn" \
  --libraries "${package_root}/libraries" \
  "$sketch_dir")
esptool_dir=$(awk -F= '$1 == "runtime.tools.esptool_py.path" { print $2; exit }' <<< "$properties")
mklittlefs_dir=$(awk -F= '$1 == "runtime.tools.mklittlefs.path" { print $2; exit }' <<< "$properties")
esptool="${esptool_dir}/esptool"
mklittlefs="${mklittlefs_dir}/mklittlefs"

for tool in "$esptool" "$mklittlefs"; do
  [[ -x "$tool" ]] || { echo "Required tool not found: ${tool}" >&2; exit 127; }
done

cp "${build_dir}/${project_name}.bin" "${output_dir}/network-radio-v3.1.0-lcd-ota.bin"
"$mklittlefs" -c "${sketch_dir}/data" -b 4096 -p 256 -s "$partition_size" \
  "${output_dir}/network-radio-v3.1.0-lcd-littlefs.bin"

"$esptool" --chip esp32s3 merge-bin \
  -o "${output_dir}/network-radio-v3.1.0-lcd-full.bin" \
  --pad-to-size 16MB --flash-mode keep --flash-freq keep --flash-size keep \
  0x0 "${build_dir}/${project_name}.bootloader.bin" \
  0x8000 "${build_dir}/${project_name}.partitions.bin" \
  0x19000 "${build_dir}/boot_app0.bin" \
  0x20000 "${build_dir}/${project_name}.bin" \
  0x620000 "${output_dir}/network-radio-v3.1.0-lcd-littlefs.bin"

if [[ ${1:-} == "--flash" ]]; then
  [[ -n ${2:-} ]] || { echo "usage: $0 [--flash /dev/cu.usbserial-XXXX]" >&2; exit 64; }
  "$esptool" --chip esp32s3 --port "$2" --baud 460800 \
    --before default-reset --after hard-reset write-flash --erase-all \
    --flash-mode dio --flash-freq 80m --flash-size 16MB \
    0x0 "${output_dir}/network-radio-v3.1.0-lcd-full.bin"
elif [[ $# -ne 0 ]]; then
  echo "usage: $0 [--flash /dev/cu.usbserial-XXXX]" >&2
  exit 64
fi

echo "Build complete: ${output_dir}"
