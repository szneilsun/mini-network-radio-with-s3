#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 BUILD_PATH PROJECT_NAME" >&2
  exit 64
fi

build_path=$1
project_name=$2
partition_csv="${build_path}/partitions.csv"
flash_args="${build_path}/flash_args"
merged_bin="${build_path}/${project_name}.merged.bin"
bootloader_bin="${build_path}/${project_name}.bootloader.bin"
partitions_bin="${build_path}/${project_name}.partitions.bin"
boot_app0_bin="${build_path}/boot_app0.bin"
app_bin="${build_path}/${project_name}.bin"

fail() {
  echo "Flash layout verification failed: $*" >&2
  exit 1
}

for artifact in \
  "$partition_csv" \
  "$flash_args" \
  "$merged_bin" \
  "$bootloader_bin" \
  "$partitions_bin" \
  "$boot_app0_bin" \
  "$app_bin"; do
  [[ -f "$artifact" ]] || fail "missing ${artifact}"
done

partition_value() {
  local name=$1
  awk -F, -v expected_name="$name" '
    /^[[:space:]]*#/ { next }
    {
      for (i = 1; i <= NF; i++) {
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", $i)
      }
      if ($1 == expected_name) {
        print tolower($4), tolower($5)
        exit
      }
    }
  ' "$partition_csv"
}

check_partition() {
  local name=$1
  local expected=$2
  local actual
  actual=$(partition_value "$name")
  [[ "$actual" == "$expected" ]] || fail "${name} is '${actual}', expected '${expected}'"
}

check_partition nvs "0x9000 0x10000"
check_partition otadata "0x19000 0x2000"
check_partition app0 "0x20000 0x300000"
check_partition app1 "0x320000 0x300000"
check_partition spiffs "0x620000 0x9d0000"
check_partition coredump "0xff0000 0x10000"

grep -Fqx "0x19000 boot_app0.bin" "$flash_args" || fail "flash_args does not place boot_app0 at 0x19000"
grep -Fqx "0x20000 ${project_name}.bin" "$flash_args" || fail "flash_args does not place the application at 0x20000"
if grep -Eiq '^0x(e000|10000)[[:space:]]' "$flash_args"; then
  fail "flash_args still contains a legacy 0xE000/0x10000 image offset"
fi

file_size() {
  if stat -f '%z' "$1" >/dev/null 2>&1; then
    stat -f '%z' "$1"
  else
    stat -c '%s' "$1"
  fi
}

compare_merged_segment() {
  local source_file=$1
  local offset=$2
  local size
  size=$(file_size "$source_file")
  cmp -s "$source_file" <(
    dd if="$merged_bin" bs=4096 skip="$((offset / 4096))" 2>/dev/null | head -c "$size"
  ) || fail "$(basename "$source_file") is not present at $(printf '0x%X' "$offset") in the merged image"
}

[[ "$(file_size "$merged_bin")" -eq 16777216 ]] || fail "merged image is not exactly 16 MiB"
compare_merged_segment "$bootloader_bin" 0x0
compare_merged_segment "$partitions_bin" 0x8000
compare_merged_segment "$boot_app0_bin" 0x19000
compare_merged_segment "$app_bin" 0x20000

legacy_byte=$(
  dd if="$merged_bin" bs=1 skip=$((0x10000)) count=1 2>/dev/null \
    | od -An -tx1 \
    | tr -d '[:space:]'
)
[[ "$legacy_byte" == "ff" ]] || fail "legacy application address 0x10000 is not erased in the merged image"

echo "Flash layout verified: otadata@0x19000, app0@0x20000, 16 MiB merged image."
