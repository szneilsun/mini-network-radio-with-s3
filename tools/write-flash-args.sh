#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 6 ]]; then
  echo "usage: $0 BUILD_PATH PROJECT_NAME BOOTLOADER_ADDR FLASH_MODE FLASH_FREQ FLASH_SIZE" >&2
  exit 64
fi

build_path=$1
project_name=$2
bootloader_addr=$3
flash_mode=$4
flash_freq=$5
flash_size=$6

printf '%s\n' \
  "--flash-mode ${flash_mode} --flash-freq ${flash_freq} --flash-size ${flash_size}" \
  "${bootloader_addr} ${project_name}.bootloader.bin" \
  "0x8000 ${project_name}.partitions.bin" \
  "0x19000 boot_app0.bin" \
  "0x20000 ${project_name}.bin" \
  > "${build_path}/flash_args"
