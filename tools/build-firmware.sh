#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
# The repository keeps one maintained sketch in sources/. ESP32_SKETCH_DIR is
# available only for temporary compatibility or regression builds.
sketch_dir=${ESP32_SKETCH_DIR:-"${project_root}/sources/esp32-network-radio"}
arduino_cli=${ARDUINO_CLI:-arduino-cli}
fqbn=${ESP32_FQBN:-"esp32:esp32:esp32s3:UploadSpeed=460800,USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,PSRAM=opi,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,ZigbeeMode=default"}
project_name="$(basename "$sketch_dir").ino"
build_path=${ESP32_BUILD_PATH:-"${project_root}/build/${project_name%.ino}"}
upload_requested=false
upload_port=
compile_args=()
ctags_args=()

while (( $# > 0 )); do
  case "$1" in
    --upload)
      upload_requested=true
      ;;
    --port)
      if (( $# < 2 )); then
        echo "--port requires a serial-device path." >&2
        exit 64
      fi
      shift
      upload_port=$1
      ;;
    --port=*)
      upload_port=${1#--port=}
      ;;
    *)
      compile_args+=("$1")
      ;;
  esac
  shift
done

[[ -x "$(command -v "$arduino_cli" 2>/dev/null)" ]] || {
  echo "arduino-cli was not found; set ARDUINO_CLI to its executable path." >&2
  exit 127
}
[[ -f "${sketch_dir}/${project_name}" ]] || {
  echo "Sketch not found: ${sketch_dir}/${project_name}" >&2
  exit 66
}
[[ -f "${sketch_dir}/partitions.csv" ]] || {
  echo "Custom partition table not found: ${sketch_dir}/partitions.csv" >&2
  exit 66
}

# Arduino's bundled ctags for this core can be x86-only on Apple Silicon.
# Prefer an installed Universal Ctags when it is available, while leaving
# other hosts on the core-provided default.
if command -v ctags >/dev/null 2>&1 && ctags --version 2>/dev/null | grep -qi 'universal ctags'; then
  ctags_args=(--build-property "runtime.tools.ctags.path=$(dirname "$(command -v ctags)")")
fi

mkdir -p "$build_path"

merge_recipe='recipe.hooks.objcopy.postobjcopy.3.pattern_args=--chip {build.mcu} merge-bin -o "{build.path}/{build.project_name}.merged.bin" --pad-to-size {build.flash_size} --flash-mode keep --flash-freq keep --flash-size keep {build.bootloader_addr} "{build.path}/{build.project_name}.bootloader.bin" 0x8000 "{build.path}/{build.project_name}.partitions.bin" 0x19000 "{runtime.platform.path}/tools/partitions/boot_app0.bin" 0x20000 "{build.path}/{build.project_name}.bin"'
flash_args_recipe="recipe.hooks.objcopy.postobjcopy.4.pattern=\"${project_root}/tools/write-flash-args.sh\" \"{build.path}\" \"{build.project_name}\" \"{build.bootloader_addr}\" \"{build.flash_mode}\" \"{build.img_freq}\" \"{build.flash_size}\""

if (( ${#compile_args[@]} > 0 )); then
  "$arduino_cli" compile \
    --fqbn "$fqbn" \
    --build-path "$build_path" \
    --build-property "$merge_recipe" \
    --build-property "$flash_args_recipe" \
    "${ctags_args[@]}" \
    "${compile_args[@]}" \
    "$sketch_dir"
else
  "$arduino_cli" compile \
    --fqbn "$fqbn" \
    --build-path "$build_path" \
    --build-property "$merge_recipe" \
    --build-property "$flash_args_recipe" \
    "${ctags_args[@]}" \
    "$sketch_dir"
fi

properties=$("$arduino_cli" compile --show-properties --fqbn "$fqbn" "$sketch_dir")
esptool_dir=$(awk -F= '$1 == "runtime.tools.esptool_py.path" { print $2; exit }' <<< "$properties")
mklittlefs_dir=$(awk -F= '$1 == "runtime.tools.mklittlefs.path" { print $2; exit }' <<< "$properties")
build_esptool="${esptool_dir}/esptool"
if [[ -n ${MKLITTLEFS:-} ]]; then
  mklittlefs=$MKLITTLEFS
elif command -v mklittlefs >/dev/null 2>&1; then
  mklittlefs=$(command -v mklittlefs)
else
  mklittlefs="${mklittlefs_dir}/mklittlefs"
fi
[[ -x "$build_esptool" && -x "$mklittlefs" ]] || {
  echo "Required ESP32 image tool was not found." >&2
  exit 127
}

littlefs_image="${build_path}/littlefs.bin"
"$mklittlefs" -c "${sketch_dir}/data" -b 4096 -p 256 -s 10289152 "$littlefs_image"

# Replace Arduino's application-only merged image with a complete 16 MiB
# recovery image that also contains the project LittleFS assets.
"$build_esptool" --chip esp32s3 merge-bin \
  -o "${build_path}/${project_name}.merged.bin" \
  --pad-to-size 16MB --flash-mode keep --flash-freq keep --flash-size keep \
  0x0 "${build_path}/${project_name}.bootloader.bin" \
  0x8000 "${build_path}/${project_name}.partitions.bin" \
  0x19000 "${build_path}/boot_app0.bin" \
  0x20000 "${build_path}/${project_name}.bin" \
  0x620000 "$littlefs_image"

"${project_root}/tools/verify-flash-layout.sh" "$build_path" "$project_name"

if [[ "$upload_requested" == true ]]; then
  [[ -n "$upload_port" ]] || {
    echo "--upload requires --port <serial-device>." >&2
    exit 64
  }

  esptool=${ESPTOOL:-}
  if [[ -z "$esptool" ]]; then
    esptool_dir=$("$arduino_cli" compile --show-properties --fqbn "$fqbn" "$sketch_dir" |
      awk -F= '$1 == "runtime.tools.esptool_py.path" { print $2; exit }')
    esptool="${esptool_dir}/esptool"
  fi
  [[ -x "$esptool" ]] || {
    echo "esptool was not found; set ESPTOOL to the executable path." >&2
    exit 127
  }

  flash_args="${build_path}/flash_args"
  flash_options=()
  write_images=()
  while IFS= read -r line; do
    [[ -n "$line" ]] || continue
    if [[ "$line" == --* ]]; then
      read -r -a flash_options <<< "$line"
    else
      read -r offset image <<< "$line"
      [[ -n "${offset:-}" && -n "${image:-}" ]] || {
        echo "Invalid flash_args entry: ${line}" >&2
        exit 65
      }
      write_images+=("$offset" "${build_path}/${image}")
    fi
  done < "$flash_args"

  write_images+=(0x620000 "$littlefs_image")
  (( ${#flash_options[@]} > 0 && ${#write_images[@]} > 0 )) || {
    echo "flash_args did not contain a write-flash layout." >&2
    exit 65
  }

  "$esptool" --chip esp32s3 --port "$upload_port" \
    --baud "${ESP32_UPLOAD_SPEED:-460800}" \
    --before default-reset --after hard-reset write-flash \
    "${flash_options[@]}" "${write_images[@]}"
fi
