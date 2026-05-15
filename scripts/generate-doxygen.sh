#!/usr/bin/env bash
# Generate Doxygen HTML documentation with doxygen-awesome-css theme.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

DOXYGEN_AWESOME_VERSION="v2.4.2"
DOXYGEN_AWESOME_BASE_URL="https://raw.githubusercontent.com/jothepro/doxygen-awesome-css/$DOXYGEN_AWESOME_VERSION"
BUILD_DIR="build/doxygen-resources"
OUTPUT_DIR="docs/doxygen"

mkdir -p "$BUILD_DIR" "$OUTPUT_DIR"

# === Download theme files if missing ===
FILES=(
  doxygen-awesome.css
  doxygen-awesome-sidebar-only.css
  doxygen-awesome-sidebar-only-darkmode-toggle.css
  doxygen-awesome-darkmode-toggle.js
  doxygen-awesome-fragment-copy-button.js
)

needed=false
for f in "${FILES[@]}"; do
  [ -f "$BUILD_DIR/$f" ] || { needed=true; break; }
done

if [ "$needed" = true ]; then
  echo "==> Downloading doxygen-awesome-css ($DOXYGEN_AWESOME_VERSION)..."
  for f in "${FILES[@]}"; do
    curl -sL -o "$BUILD_DIR/$f" "$DOXYGEN_AWESOME_BASE_URL/$f"
  done
fi

# === Generate custom header with JS extensions ===
HEADER_FILE="$BUILD_DIR/header.html"
if [ ! -f "$HEADER_FILE" ]; then
  echo "==> Generating custom header template..."
  doxygen -w html "$HEADER_FILE" /dev/null /dev/null

  # Inject dark mode toggle + fragment copy button before </head>
  # Init class names are documented at:
  #   https://jothepro.github.io/doxygen-awesome-css/md_docs_2extensions.html
  sed -i 's|</head>|\
<script type="text/javascript" src="$relpath^doxygen-awesome-darkmode-toggle.js"></script>\
<script type="text/javascript" src="$relpath^doxygen-awesome-fragment-copy-button.js"></script>\
<script type="text/javascript">DoxygenAwesomeDarkModeToggle.init()</script>\
<script type="text/javascript">DoxygenAwesomeFragmentCopyButton.init()</script>\
</head>|' "$HEADER_FILE"
fi

# === Generate docs ===
echo "==> Generating API docs..."
doxygen Doxyfile
echo "Done. Open file://$REPO_ROOT/$OUTPUT_DIR/html/index.html in a browser."