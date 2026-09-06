#!/usr/bin/env bash
# Generate Doxygen HTML documentation with doxygen-awesome-css theme.
# Markdown is staged into build/docs/ by scripts/stage-docs.py first (YAML
# highlighting, page labels, subpage trees -- see that file) before Doxygen runs.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

# === Configuration ===
# doxygen 1.17.0 dropped the bundled jQuery and added native Mermaid rendering; 1.18.0 is the
# pin. A bump here is the only version lever -- CI never installs the runner's apt doxygen.
DOXYGEN_VERSION="1.18.0"
DOXYGEN_RELEASE_TAG="Release_$(echo "$DOXYGEN_VERSION" | tr '.' '_')"
DOXYGEN_BIN_DIR="build/doxygen-resources"
DOXYGEN_BIN="$DOXYGEN_BIN_DIR/doxygen"
# Records the version of the cached binary so a bump re-downloads instead of silently reusing a
# stale one (CI runners start clean, but developer machines keep build/ across bumps).
DOXYGEN_VERSION_FILE="$DOXYGEN_BIN_DIR/.doxygen-version"

# Determine platform-specific download URL + the sha256 of that exact asset.
DOXYGEN_BASE="https://github.com/doxygen/doxygen/releases/download/${DOXYGEN_RELEASE_TAG}"
case "$(uname -s)-$(uname -m)" in
  Linux-*)
    DOXYGEN_URL="${DOXYGEN_BASE}/doxygen-${DOXYGEN_VERSION}.linux.bin.tar.gz"
    DOXYGEN_SHA256="14fa81bdc34171edb5f1f02b1d60e74802f0439b77fa44e592565d517d72df90" ;;
  Darwin-arm64)
    DOXYGEN_URL="${DOXYGEN_BASE}/doxygen-${DOXYGEN_VERSION}-mac-arm.zip"
    DOXYGEN_SHA256="" ;;
  Darwin-x86_64)
    DOXYGEN_URL="${DOXYGEN_BASE}/doxygen-${DOXYGEN_VERSION}-mac-intel.zip"
    DOXYGEN_SHA256="" ;;
  *)  echo "ERROR: unsupported platform $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac

DOXYGEN_AWESOME_VERSION="v2.4.2"
DOXYGEN_AWESOME_BASE_URL="https://raw.githubusercontent.com/jothepro/doxygen-awesome-css/$DOXYGEN_AWESOME_VERSION"

# doxygen >= 1.17 no longer ships jQuery, but doxygen-awesome-css v2.4.2's darkmode-toggle,
# paragraph-link and fragment-copy-button scripts still call $(...) -- vendor it so
# doxygen-custom/header.html's <script src="jquery.js"> keeps resolving.
JQUERY_VERSION="3.7.1"
JQUERY_URL="https://code.jquery.com/jquery-${JQUERY_VERSION}.min.js"

# Self-contained Mermaid browser bundle (the UMD build -- one file, unlike the code-split
# dist/mermaid.esm.min.mjs). header.html loads it lazily, only on pages that contain a diagram;
# vendored so the published site has no CDN dependency.
MERMAID_VERSION="11.17.2"
MERMAID_URL="https://cdn.jsdelivr.net/npm/mermaid@${MERMAID_VERSION}/dist/mermaid.min.js"

OUTPUT_DIR="docs/doxygen"

# sha256 of every third-party web asset copied into the generated site. Several are executable
# JS shipped to readers, so a silent upstream change must fail the build. Regenerate with
# `sha256sum build/doxygen-resources/<file>` after a deliberate version bump.
RESOURCE_CHECKSUMS="
doxygen-awesome.css                                5ec49e2dfd097f6b5384e3aae0476eab47748e311fc70e207925f8fcc37477b9
doxygen-awesome-sidebar-only.css                   dc7ddd235375b71ecb0af920faa6b925ee9445ac617f3bc962b0b0db97da7b4f
doxygen-awesome-sidebar-only-darkmode-toggle.css   c1939ca910d2282068482abc72e9edcf9835e4de153ebe8b428cbace92ed4c2c
doxygen-awesome-darkmode-toggle.js                 de752867789ed21154983c22ef34441137b4cc558d5a2f92013f5b894483e5a4
doxygen-awesome-fragment-copy-button.js            009b4c9982c18bc68c6366321298316e9054a620e37b99de1276ff6a1e2c65a0
doxygen-awesome-paragraph-link.js                  f9fe333b516cdc259a25475b0ca472e8e091fd7abf9020e54949c4677a7a427f
doxygen-awesome-interactive-toc.js                 a7d6a4d59809b650afd011af6fc8805075aeb5e310940fb9583a42652fe87ba8
doxygen-awesome-tabs.js                            805b4dd5371a0c602ae112deb698e84a5bed7af3d78ba76cde8022229a893542
jquery.js                                          fc9a93dd241f6b045cbff0481cf4e1901becd0e12fb45166a8f17f95823f0b1a
mermaid.min.js                                     581ed7d74bd9048d0e3a91363927d72ef22942d7722546b27f7cc29e35390eb8
"

# Portable sha256 (Linux coreutils vs macOS): print the hex digest of $1.
sha256_hex() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1
  fi
}

# Verify a downloaded file in $DOXYGEN_BIN_DIR against RESOURCE_CHECKSUMS; abort on any mismatch
# or any file with no pinned hash.
verify_resource() {
  local name="$1" want actual
  want="$(printf '%s\n' "$RESOURCE_CHECKSUMS" | awk -v n="$name" '$1 == n { print $2 }')"
  if [ -z "$want" ]; then
    echo "ERROR: no pinned sha256 for $name -- add one to RESOURCE_CHECKSUMS" >&2
    exit 1
  fi
  actual="$(sha256_hex "$DOXYGEN_BIN_DIR/$name")"
  if [ "$actual" != "$want" ]; then
    echo "ERROR: sha256 mismatch for $name" >&2
    echo "  expected $want" >&2
    echo "  actual   $actual" >&2
    exit 1
  fi
}

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

# === Locate or download doxygen binary ===
cached_version=""
[ -f "$DOXYGEN_VERSION_FILE" ] && cached_version="$(cat "$DOXYGEN_VERSION_FILE")"
if [ -f "$DOXYGEN_BIN" ] && [ "$cached_version" = "$DOXYGEN_VERSION" ]; then
  DOXYGEN_CMD="$DOXYGEN_BIN"
else
  echo "==> Downloading doxygen ($DOXYGEN_VERSION)..."
  tmp_archive="$(mktemp)"
  curl -sL -o "$tmp_archive" "$DOXYGEN_URL"
  if [ -n "$DOXYGEN_SHA256" ]; then
    got="$(sha256_hex "$tmp_archive")"
    if [ "$got" != "$DOXYGEN_SHA256" ]; then
      echo "ERROR: sha256 mismatch for the doxygen archive" >&2
      echo "  expected $DOXYGEN_SHA256" >&2
      echo "  actual   $got" >&2
      rm -f "$tmp_archive"
      exit 1
    fi
  fi
  tmp_dir="$(mktemp -d)"
  case "$DOXYGEN_URL" in
    *.tar.gz) tar -xzf "$tmp_archive" -C "$tmp_dir" ;;
    *.zip)    unzip -q "$tmp_archive" -d "$tmp_dir" ;;
  esac
  # Find the doxygen binary in the extracted tree
  found="$(find "$tmp_dir" -maxdepth 4 -type f -name doxygen ! -name '*.h' | head -n1)"
  if [ -z "$found" ]; then
    echo "ERROR: could not find doxygen binary in downloaded archive" >&2
    rm -rf "$tmp_dir" "$tmp_archive"
    exit 1
  fi
  mv "$found" "$DOXYGEN_BIN"
  chmod +x "$DOXYGEN_BIN"
  echo "$DOXYGEN_VERSION" > "$DOXYGEN_VERSION_FILE"
  rm -rf "$tmp_dir" "$tmp_archive"
  DOXYGEN_CMD="$DOXYGEN_BIN"
fi

# === Download + verify theme and vendored JS ===
# name -> download URL for every file listed in RESOURCE_CHECKSUMS.
resource_url() {
  case "$1" in
    jquery.js)      echo "$JQUERY_URL" ;;
    mermaid.min.js) echo "$MERMAID_URL" ;;
    *)              echo "$DOXYGEN_AWESOME_BASE_URL/$1" ;;
  esac
}

RESOURCE_FILES="$(printf '%s\n' "$RESOURCE_CHECKSUMS" | awk 'NF { print $1 }')"
for f in $RESOURCE_FILES; do
  if [ ! -f "$DOXYGEN_BIN_DIR/$f" ]; then
    echo "==> Downloading $f..."
    curl -sL -o "$DOXYGEN_BIN_DIR/$f" "$(resource_url "$f")"
  fi
  verify_resource "$f"
done

# === Stage Markdown into build/docs/ (YAML highlighting, labels, subpage trees) ===
echo "==> Staging Markdown docs..."
PYTHON_CMD="python3"
"$PYTHON_CMD" "$SCRIPT_DIR/stage-docs.py"
echo ""

# === Generate docs ===
echo "==> Generating API docs..."
rm -rf "$OUTPUT_DIR/html"
rm -f "$OUTPUT_DIR"/*.tag
"$DOXYGEN_CMD" Doxyfile

# === Crawl the generated site for broken links ===
# scripts/check-docs-links.py checks the source tree (GitHub render); this checks the built
# HTML (Pages render). Kept here rather than in `make lint` because it needs the output.
echo "==> Crawling generated docs for broken links..."
"$PYTHON_CMD" "$SCRIPT_DIR/check-doxygen-output-links.py"

echo "Done. Open file://$REPO_ROOT/$OUTPUT_DIR/html/index.html in a browser."
