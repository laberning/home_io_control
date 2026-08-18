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

# Build directory. compile_commands.json comes from ESP-IDF's own CMake build (it sets
# CMAKE_EXPORT_COMPILE_COMMANDS on by default), not from a separate `pio -t compiledb` step —
# current pioarduino ESP-IDF builds no longer scaffold a platformio.ini project file for `pio`
# to operate on, so that used to be the approach but now fails with NotPlatformIOProjectError.
CONFIG_DIR=$(dirname "$CONFIG_FILE")
BUILD_DIR="${CONFIG_DIR}/.esphome/build/${DEVICE_NAME}"
COMPILE_DB="${BUILD_DIR}/build/compile_commands.json"
RAW_DB="build/clang-tidy/compile_commands.raw.json"
FILTERED_DB="build/clang-tidy/compile_commands.json"
COMPONENT_DIR="$REPO_ROOT/components/home_io_control"
GENERATED_COMPONENT_PREFIX="src/esphome/components/home_io_control/"

# Ubuntu's clang build lists an Xtensa backend but does not accept the ESP-IDF
# Xtensa target triple. A generic 32-bit target keeps pointer-width diagnostics
# in ESP-IDF headers consistent with ESP32 while clang-tidy analyzes our code.
TARGET_TRIPLE="${CLANG_TIDY_TARGET:-i386-pc-linux-gnu}"

mkdir -p "$(dirname "$FILTERED_DB")"

# Catch a stale/half-regenerated build cache before it turns into a confusing missing-vtable
# link error further down (see scripts/check-build-cache.py for the full story).
python3 "$SCRIPT_DIR/check-build-cache.py" --clean

# Build inside Docker if compile_commands.json missing
if [ ! -f "$COMPILE_DB" ]; then
  echo "==> Building $CONTAINER_CONFIG inside Docker..."
  docker compose run --rm esphome compile "$CONTAINER_CONFIG"
fi

# ESPHome's generated api/proto.h makes ProtoMessage's destructor public only under
# `#ifndef USE_HOST`, specifically so value-initializing std::array<DeviceInfo, N> members
# (e.g. api_pb2.h's DeviceInfoResponse::devices, added by this project's `esphome: devices:`
# usage) doesn't hit a "protected destructor" error under clang -- but that guard only covers
# ESPHome's own host-target test builds, not this script's clang-substituted-for-gcc analysis of
# an ESP32-target build. Extend the guard to also honor CLANG_TIDY, matching the macro name
# ESPHome's own components/spi/spi.h already uses for the same kind of clang-only concession, and
# define it below (only for this analysis pass -- never for the real GCC/ESP-IDF firmware build).
PROTO_H="$BUILD_DIR/src/esphome/components/api/proto.h"
if [ -f "$PROTO_H" ]; then
  # Written root-owned by the esphome container's bind mount -- a host-side `sed -i` can't write
  # there (it errors on the temp file it needs to create alongside it), so patch it from inside
  # the same container instead.
  CONTAINER_PROTO_H="/${PROTO_H#./}"
  docker compose run --rm --entrypoint sh esphome -c \
    "sed -i 's/^#ifndef USE_HOST\$/#if !defined(USE_HOST) \&\& !defined(CLANG_TIDY)/' '$CONTAINER_PROTO_H'"
fi

# Derive the toolchain root from whichever compiler this build actually used, rather than
# hardcoding a path: pioarduino has relocated the xtensa-esp-elf toolchain's install location
# more than once (config/.pio/packages/ -> config/.pio/tools/ -> config/.esphome/idf/tools/
# xtensa-esp-elf/<version>/), and a stale hardcoded path here used to silently degrade to
# clang-tidy running with no toolchain headers at all (missing -isystem dirs just get skipped
# below) instead of failing loudly.
COMPILER_PATH=$(grep -m1 -oE '/[^"[:space:]]*xtensa-esp32-elf-g\+\+' "$COMPILE_DB" || true)
if [ -z "$COMPILER_PATH" ]; then
  echo "ERROR: Could not find xtensa-esp32-elf-g++ in $COMPILE_DB"
  exit 1
fi
TOOLCHAIN_PKG_ROOT="$REPO_ROOT${COMPILER_PATH%/bin/xtensa-esp32-elf-g++}"
XTENSA_TOOLCHAIN="$TOOLCHAIN_PKG_ROOT/xtensa-esp-elf"
PICOLIBC_TOOLCHAIN="$TOOLCHAIN_PKG_ROOT/picolibc/xtensa-esp-elf"
if [ ! -d "$XTENSA_TOOLCHAIN" ]; then
  echo "ERROR: Resolved toolchain root does not exist: $TOOLCHAIN_PKG_ROOT"
  echo "       (derived from compiler path $COMPILER_PATH in $COMPILE_DB)"
  exit 1
fi

# ESP-IDF's ninja build passes some C++ flags via a compiler response file (@"path") instead of
# inline on the command line. Inline its content below — both because clang (run on the host)
# can't resolve a container-only path, and so the GCC-specific-flag filtering that follows can
# see and strip flags like -mlongcalls that live inside it instead of on the command line.
# Match specifically on "cxxflags": the database also has a separate "cflags" response file used
# by plain-C translation units elsewhere in the tree, which isn't the one our .cpp files use.
RESPONSE_FILE_CONTAINER_PATH=$(grep -m1 -oE '@\\"[^\\]*cxxflags\\"' "$COMPILE_DB" | sed -e 's/^@\\"//' -e 's/\\"$//' || true)
RESPONSE_FILE_INLINE_EXPR="s/PLACEHOLDER_NEVER_MATCHES//"
if [ -n "$RESPONSE_FILE_CONTAINER_PATH" ]; then
  RESPONSE_FILE_HOST_PATH="$REPO_ROOT$RESPONSE_FILE_CONTAINER_PATH"
  if [ -f "$RESPONSE_FILE_HOST_PATH" ]; then
    RESPONSE_FILE_FLAGS=$(tr '\n' ' ' < "$RESPONSE_FILE_HOST_PATH")
    RESPONSE_FILE_INLINE_EXPR="s|@\\\\\"${RESPONSE_FILE_CONTAINER_PATH}\\\\\"|${RESPONSE_FILE_FLAGS}|g"
  fi
fi

# Filter GCC-specific flags clang can't handle
echo "==> Filtering compile_commands.json -> $RAW_DB"
sed -e "$RESPONSE_FILE_INLINE_EXPR" \
    -e 's/ -mlongcalls//g' \
    -e 's/ -fno-tree-switch-conversion//g' \
    -e 's/ -fstrict-volatile-bitfields//g' \
    -e 's/ -freorder-blocks//g' \
    -e 's/ -fno-jump-tables//g' \
  "$COMPILE_DB" > "$RAW_DB"

# Translate Docker paths to host paths
HOST_CONFIG_DIR="$REPO_ROOT/config"
echo "==> Adjusting paths: /config/ -> $HOST_CONFIG_DIR/"
sed \
    -e "s|\"directory\": \"/config/|\"directory\": \"${HOST_CONFIG_DIR}/|" \
    -e "s|-I/config/|-I${HOST_CONFIG_DIR}/|g" \
    -e "s| /config/| ${HOST_CONFIG_DIR}/|g" \
    -e "s|^/config/|${HOST_CONFIG_DIR}/|" \
  "$RAW_DB" > "$RAW_DB.tmp" && mv "$RAW_DB.tmp" "$RAW_DB"

# Replace cross-compiler with clang++ (full absolute path or bare name, depending on how the
# build generated this compile database)
echo "==> Replacing cross-compiler with clang++..."
sed 's|"command": "[^"]*xtensa-esp32-elf-g++|"command": "clang++|' "$RAW_DB" > "$RAW_DB.tmp" && mv "$RAW_DB.tmp" "$RAW_DB"

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
# ESPHome copy. Updating only the "file" field is not enough for clang-tidy — it reads the
# actual analyzed file from the "-c" argument inside "command".
#
# Match the whole space/quote-delimited token ending in the generated-copy suffix, not just a
# literal leading space before it: since compile_commands.json switched to CMake-native absolute
# paths, the "-c" argument looks like ".../config/tests/.esphome/build/<env>/src/esphome/..."
# rather than "/config/src/esphome/...", so the generated-path segment is no longer preceded by
# a bare space (it's preceded by the rest of that absolute path). A leading-space anchor here
# used to silently fail to match, leaving clang-tidy analyzing a stale generated copy instead of
# the live edit whenever compile_commands.json wasn't regenerated by a fresh Docker build first.
for src_file in "$COMPONENT_DIR"/*.cpp; do
  base_name=$(basename "$src_file")
  sed "s|[^\"[:space:]]*${GENERATED_COMPONENT_PREFIX}${base_name}|${src_file}|g" "$FILTERED_DB" > "$FILTERED_DB.tmp" && mv "$FILTERED_DB.tmp" "$FILTERED_DB"
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
  -DCLANG_TIDY
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
FILES_ARRAY=()
while IFS= read -r f; do
  FILES_ARRAY+=("$f")
done < <(find "$COMPONENT_DIR" -type f -name '*.cpp' | sort)
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
