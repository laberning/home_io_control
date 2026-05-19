#!/usr/bin/env bash
# ESPHome clang-tidy integration, inspired by
# https://github.com/esphome/esphome/blob/dev/script/clang-tidy
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

# Config file to build (default: stable test config)
CONFIG_FILE="${1:-config/tests/test-esp32-esp-idf.yaml}"
# Container path: host's config/... maps to /config/...
CONTAINER_CONFIG="/${CONFIG_FILE}"

# Extract device_name from YAML substitutions
DEVICE_NAME=$(grep 'device_name:' "$CONFIG_FILE" | head -1 | cut -d: -f2- | sed "s/['\"]//g" | xargs)
if [ -z "$DEVICE_NAME" ]; then
  echo "ERROR: Could not extract device_name from $CONFIG_FILE"
  exit 1
fi
echo "==> Using device_name: $DEVICE_NAME"

# Build directory (where .esphome places compiledb)
CONFIG_DIR=$(dirname "$CONFIG_FILE")
BUILD_DIR="${CONFIG_DIR}/.esphome/build/${DEVICE_NAME}"
COMPILE_DB="${BUILD_DIR}/compile_commands.json"
RAW_DB="build/clang-tidy/compile_commands.raw.json"
FILTERED_DB="build/clang-tidy/compile_commands.json"
COMPONENT_DIR="$REPO_ROOT/components/home_io_control"
GENERATED_COMPONENT_PREFIX="src/esphome/components/home_io_control/"

# Toolchain package root
TOOLCHAIN_PKG_ROOT="$REPO_ROOT/config/.pio/packages/toolchain-xtensa-esp-elf"
XTENSA_TOOLCHAIN="$TOOLCHAIN_PKG_ROOT/xtensa-esp-elf"
PICOLIBC_TOOLCHAIN="$TOOLCHAIN_PKG_ROOT/picolibc/xtensa-esp-elf"
# Ubuntu's clang build lists an Xtensa backend but does not accept the ESP-IDF
# Xtensa target triple. A generic 32-bit target keeps pointer-width diagnostics
# in ESP-IDF headers consistent with ESP32 while clang-tidy analyzes our code.
TARGET_TRIPLE="${CLANG_TIDY_TARGET:-i386-pc-linux-gnu}"

mkdir -p "$(dirname "$FILTERED_DB")"

# Build inside Docker if compile_commands.json missing
if [ ! -f "$COMPILE_DB" ]; then
  echo "==> Building $CONTAINER_CONFIG inside Docker..."
  docker compose run --rm esphome compile "$CONTAINER_CONFIG"
  echo "==> Generating compile_commands.json for environment: $DEVICE_NAME"
  docker compose run --rm --entrypoint sh esphome -c \
    "cd /${BUILD_DIR} && pio run -t compiledb -e ${DEVICE_NAME}"
fi

# Filter GCC-specific flags clang can't handle
echo "==> Filtering compile_commands.json -> $RAW_DB"
sed -e 's/ -mlongcalls//g' \
    -e 's/ -fno-tree-switch-conversion//g' \
    -e 's/ -fstrict-volatile-bitfields//g' \
    -e 's/ -freorder-blocks//g' \
    -e 's/ -fno-jump-tables//g' \
  "$COMPILE_DB" > "$RAW_DB"

# Translate Docker paths to host paths
HOST_CONFIG_DIR="$REPO_ROOT/config"
echo "==> Adjusting paths: /config/ -> $HOST_CONFIG_DIR/"
sed -i -e "s|\"directory\": \"/config/|\"directory\": \"${HOST_CONFIG_DIR}/|" \
       -e "s|-I/config/|-I${HOST_CONFIG_DIR}/|g" \
       -e "s| /config/| ${HOST_CONFIG_DIR}/|g" \
       -e "s|^/config/|${HOST_CONFIG_DIR}/|" \
  "$RAW_DB"

# Replace cross-compiler with clang++
echo "==> Replacing cross-compiler with clang++..."
sed -i 's|"command": "xtensa-esp32-elf-g++|"command": "clang++|' "$RAW_DB"

# Keep only this component's translation units and map them back to repository sources.
echo "==> Reducing compile_commands.json to project sources..."
{
  printf '[\n'
  awk \
    -v prefix="$GENERATED_COMPONENT_PREFIX" \
    -v component_dir="$COMPONENT_DIR" '
      function flush_record(    relative_path, mapped_path) {
        if (!in_object)
          return
        if (file_path ~ prefix) {
          relative_path = file_path
          sub("^.*" prefix, "", relative_path)
          mapped_path = component_dir "/" relative_path
          gsub(/"file": "[^"]*"/, "\"file\": \"" mapped_path "\"", record)
          sub(/,[[:space:]]*$/, "", record)
          if (count > 0)
            printf(",\n")
          printf("%s", record)
          count++
        }
        record = ""
        file_path = ""
        in_object = 0
      }

      BEGIN {
        count = 0
        in_object = 0
      }

      /^[[:space:]]*\{$/ {
        flush_record()
        in_object = 1
        record = $0 ORS
        next
      }

      {
        if (!in_object)
          next
        record = record $0 ORS
        if (match($0, /"file": "[^"]*"/)) {
          file_path = substr($0, RSTART + 9, RLENGTH - 10)
        }
        if ($0 ~ /^[[:space:]]*}[,]?$/)
          flush_record()
      }

      END {
        flush_record()
      }
    ' "$RAW_DB"
  printf ']\n'
} > "$FILTERED_DB"

RETAINED_COUNT=$(grep -c '"file": "' "$FILTERED_DB" || true)
echo "Retained ${RETAINED_COUNT} project translation units"

# Point each compile command at the repository source file instead of the generated
# ESPHome copy. Updating only the "file" field is not enough for clang-tidy.
for src_file in "$COMPONENT_DIR"/*.cpp; do
  base_name=$(basename "$src_file")
  sed -i "s| src/esphome/components/home_io_control/${base_name}\"| ${src_file}\"|g" "$FILTERED_DB"
done

if [ ! -s "$FILTERED_DB" ]; then
  echo "ERROR: Filtered compile database is empty"
  exit 1
fi

# Build clang-tidy extra args
EXTRA_ARGS=(
  --target="$TARGET_TRIPLE"
  -nostdinc
  -nostdinc++
  -D__XTENSA__
  -D_LIBC
)

# Add toolchain system include directories
echo "==> Adding toolchain includes..."
for inc_dir in \
  "$XTENSA_TOOLCHAIN/sys-include" \
  "$XTENSA_TOOLCHAIN/include" \
  "$PICOLIBC_TOOLCHAIN/sys-include" \
  "$PICOLIBC_TOOLCHAIN/include"
do
  [ -d "$inc_dir" ] && EXTRA_ARGS+=( -isystem "$inc_dir" )
done

# Add C++ standard library includes
CXX_VER=$(ls "$XTENSA_TOOLCHAIN/include/c++" 2>/dev/null | head -n1 || true)
if [ -n "$CXX_VER" ]; then
  CXX_PATH="$XTENSA_TOOLCHAIN/include/c++/$CXX_VER"
  [ -d "$CXX_PATH" ] && EXTRA_ARGS+=( -isystem "$CXX_PATH" )
  for tdir in xtensa-esp-elf esp32; do
    [ -d "$CXX_PATH/$tdir" ] && EXTRA_ARGS+=( -isystem "$CXX_PATH/$tdir" )
  done
fi

# Add GCC private headers (e.g., stddef.h)
STDDEF_FILE=$(find "$TOOLCHAIN_PKG_ROOT" -name stddef.h -type f 2>/dev/null | head -n1 || true)
if [ -n "$STDDEF_FILE" ]; then
  STDDEF_DIR=$(dirname "$STDDEF_FILE")
  [ -d "$STDDEF_DIR" ] && EXTRA_ARGS+=( -isystem "$STDDEF_DIR" )
fi

# Convert to -extra-arg format for clang-tidy
CLANG_TIDY_EXTRA=()
for a in "${EXTRA_ARGS[@]}"; do
  CLANG_TIDY_EXTRA+=( -extra-arg="$a" )
done

# Collect translation units.
mapfile -t FILES_ARRAY < <(find "$COMPONENT_DIR" -type f -name '*.cpp' | sort)
if [ ${#FILES_ARRAY[@]} -eq 0 ]; then
  echo "No source files found in components/home_io_control"
  exit 1
fi

HEADER_FILTER='(^|.*/)components/home_io_control/.*'
JOBS="${CLANG_TIDY_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 4)}"

filter_tidy_output() {
  local line=""
  local file_path=""

  while IFS= read -r line; do
    if [[ "$line" =~ ^[0-9]+[[:space:]]warnings?[[:space:]]generated\.$ ]]; then
      continue
    fi
    if [[ "$line" =~ ^[0-9]+[[:space:]]warnings?[[:space:]]and[[:space:]][0-9]+[[:space:]]errors?[[:space:]]generated\.$ ]]; then
      continue
    fi
    if [[ "$line" =~ ^Suppressed[[:space:]][0-9]+[[:space:]]warnings?[[:space:]]\([0-9]+[[:space:]]in[[:space:]]non-user[[:space:]]code\)\.$ ]]; then
      continue
    fi
    if [[ "$line" == *clang-tidy* ]] && [[ "$line" =~ (/[^[:space:]]+\.(c|cc|cpp|cxx|h|hpp))$ ]]; then
      file_path="${BASH_REMATCH[1]}"
      file_path="${file_path#"$REPO_ROOT"/}"
      printf '==> Analyzing %s\n' "$file_path"
      continue
    fi
    if [[ -n "$line" ]]; then
      printf '%s\n' "$line"
    fi
  done
}

echo "==> Running clang-tidy on ${#FILES_ARRAY[@]} translation units (device: $DEVICE_NAME, jobs: $JOBS)..."
if command -v run-clang-tidy >/dev/null 2>&1; then
  set +e
  run-clang-tidy \
    -p "$(dirname "$FILTERED_DB")" \
    -j "$JOBS" \
    -header-filter "$HEADER_FILTER" \
    -use-color \
    -quiet \
    "${CLANG_TIDY_EXTRA[@]}" 2>&1 | filter_tidy_output
  status=${PIPESTATUS[0]}
  set -e
  if [ "$status" -ne 0 ]; then
    exit "$status"
  fi
else
  clang-tidy \
    -p "$(dirname "$FILTERED_DB")" \
    --header-filter "$HEADER_FILTER" \
    --use-color \
    "${CLANG_TIDY_EXTRA[@]}" \
    "${FILES_ARRAY[@]}"
fi

echo "Done."
