#!/usr/bin/env bash
# ESPHome clang-tidy integration, inspired by
# https://github.com/esphome/esphome/blob/dev/script/clang-tidy
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

# Config file to build (default: stable test config)
CONFIG_FILE="${1:-config/tests/test.esp32-esp-idf.yaml}"
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
FILTERED_DB="build/clang-tidy/compile_commands.json"

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
echo "==> Filtering compile_commands.json -> $FILTERED_DB"
sed -e 's/ -mlongcalls//g' \
    -e 's/ -fno-tree-switch-conversion//g' \
    -e 's/ -fstrict-volatile-bitfields//g' \
    -e 's/ -freorder-blocks//g' \
    -e 's/ -fno-jump-tables//g' \
    "$COMPILE_DB" > "$FILTERED_DB"

# Translate Docker paths to host paths
HOST_CONFIG_DIR="$REPO_ROOT/config"
echo "==> Adjusting paths: /config/ -> $HOST_CONFIG_DIR/"
sed -i -e "s|\"directory\": \"/config/|\"directory\": \"${HOST_CONFIG_DIR}/|" \
       -e "s|-I/config/|-I${HOST_CONFIG_DIR}/|g" \
       -e "s| /config/| ${HOST_CONFIG_DIR}/|g" \
       -e "s|^/config/|${HOST_CONFIG_DIR}/|" \
       "$FILTERED_DB"

# Replace cross-compiler with clang++
echo "==> Replacing cross-compiler with clang++..."
sed -i 's|"command": "xtensa-esp32-elf-g++|"command": "clang++|' "$FILTERED_DB"

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
mapfile -t FILES_ARRAY < <(find components/home_io_control -type f \( -name '*.cpp' -o -name '*.h' \))
if [ ${#FILES_ARRAY[@]} -eq 0 ]; then
  echo "No source files found in components/home_io_control"
  exit 1
fi

echo "==> Running clang-tidy on ${#FILES_ARRAY[@]} files (device: $DEVICE_NAME)..."
clang-tidy \
  -p "$FILTERED_DB" \
  "${CLANG_TIDY_EXTRA[@]}" \
  "${FILES_ARRAY[@]}"

echo "Done."
