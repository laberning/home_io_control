#!/usr/bin/env bash
# Generate Doxygen HTML documentation with doxygen-awesome-css theme.
# YAML code blocks in Markdown are pre-processed by scripts/pygmentize-md-yaml.py
# to produce Pygments HTML before Doxygen runs.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

# === Configuration ===
DOXYGEN_VERSION="1.16.1"
DOXYGEN_RELEASE_TAG="Release_$(echo "$DOXYGEN_VERSION" | tr '.' '_')"
DOXYGEN_URL="https://github.com/doxygen/doxygen/releases/download/${DOXYGEN_RELEASE_TAG}/doxygen-${DOXYGEN_VERSION}.linux.bin.tar.gz"
DOXYGEN_BIN_DIR="build/doxygen-resources"
DOXYGEN_BIN="$DOXYGEN_BIN_DIR/doxygen"

DOXYGEN_AWESOME_VERSION="v2.4.2"
DOXYGEN_AWESOME_BASE_URL="https://raw.githubusercontent.com/jothepro/doxygen-awesome-css/$DOXYGEN_AWESOME_VERSION"
OUTPUT_DIR="docs/doxygen"

check_dependency() {
  local command_name="$1"
  local install_hint="$2"

  if ! command -v "$command_name" >/dev/null 2>&1; then
    MISSING_DEPENDENCIES+=("  - $command_name ($install_hint)")
  fi
}

MISSING_DEPENDENCIES=()
check_dependency "python3" "install python3"
check_dependency "pygmentize" "install python3-pygments"
check_dependency "dot" "install graphviz"

if [[ ${#MISSING_DEPENDENCIES[@]} -gt 0 ]]; then
  echo "Error: missing required documentation dependencies:" >&2
  printf '%s\n' "${MISSING_DEPENDENCIES[@]}" >&2
  echo >&2
  echo "On Debian/Ubuntu install them with:" >&2
  echo "  sudo apt-get install -y python3 python3-pygments graphviz" >&2
  exit 1
fi

mkdir -p "$DOXYGEN_BIN_DIR" "$OUTPUT_DIR"

# === Download doxygen binary if missing ===
if [ ! -f "$DOXYGEN_BIN" ]; then
  echo "==> Downloading doxygen ($DOXYGEN_VERSION)..."
  tmp_tar="$(mktemp)"
  curl -sL -o "$tmp_tar" "$DOXYGEN_URL"
  tmp_dir="$(mktemp -d)"
  tar -xzf "$tmp_tar" -C "$tmp_dir"
  # Move the extracted binary into place (handles tarballs that embed a versioned directory)
  found="$(find "$tmp_dir" -maxdepth 3 -type f -name doxygen | head -n1)"
  mv "$found" "$DOXYGEN_BIN"
  chmod +x "$DOXYGEN_BIN"
  rm -rf "$tmp_dir" "$tmp_tar"
fi

DOXYGEN_CMD="$DOXYGEN_BIN"

# === Download theme files if missing ===
FILES=(
  doxygen-awesome.css
  doxygen-awesome-sidebar-only.css
  doxygen-awesome-sidebar-only-darkmode-toggle.css
  doxygen-awesome-darkmode-toggle.js
  doxygen-awesome-fragment-copy-button.js
  doxygen-awesome-paragraph-link.js
  doxygen-awesome-interactive-toc.js
  doxygen-awesome-tabs.js
)

needed=false
for f in "${FILES[@]}"; do
  [ -f "$DOXYGEN_BIN_DIR/$f" ] || { needed=true; break; }
done

if [ "$needed" = true ]; then
  echo "==> Downloading doxygen-awesome-css ($DOXYGEN_AWESOME_VERSION)..."
  for f in "${FILES[@]}"; do
    curl -sL -o "$DOXYGEN_BIN_DIR/$f" "$DOXYGEN_AWESOME_BASE_URL/$f"
  done
fi

# === Pre-process Markdown: yaml → Pygments HTML ===
echo "==> Highlighting YAML blocks in Markdown..."
PYTHON_CMD="python3"
"$PYTHON_CMD" "$SCRIPT_DIR/pygmentize-md-yaml.py"
echo ""

# === Generate docs ===
echo "==> Generating API docs..."
rm -rf "$OUTPUT_DIR/html"
rm -f "$OUTPUT_DIR"/*.tag
"$DOXYGEN_CMD" Doxyfile
echo "Done. Open file://$REPO_ROOT/$OUTPUT_DIR/html/index.html in a browser."