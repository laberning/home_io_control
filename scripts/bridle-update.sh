#!/usr/bin/env bash
# Take a Bridle release into this repository: the generated C++ headers, the file BRIDLE_VERSION that
# names the release, and the pin of the Python wheel in scripts/requirements-dev.txt.
#
#   scripts/bridle-update.sh --dist DIR     the files of a local `make release` (DIR/dist of a Bridle checkout)
#   scripts/bridle-update.sh TAG            download the release TAG (bridle-vX.Y.Z) from GitHub
#   scripts/bridle-update.sh --verify TAG   download the release TAG and compare it with what is pinned here;
#                                           changes nothing
#   scripts/bridle-update.sh --check        offline: the committed headers and the wheel pin match BRIDLE_VERSION,
#                                           and its protocol= is the header's PROTO_MAJOR.PROTO_MINOR
#                                           (make bridle-check)
#
# Every file is checked against the release's SHA256SUMS before it is used. The headers are generated:
# never edit them, run this script. `make bridle-check` recomputes their hashes.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDLE_REPO="laberning/bridle"
BRIDLE_CHECKOUT="${BRIDLE_CHECKOUT:-$REPO_ROOT/../bridle}"  # --dist only: where the tag's commit id is read from

# === Where this repository keeps the release ===
ENUMS_DEST="components/home_io_control/radio_bridle_enums.h"      # the header the firmware includes
TEST_DIR="tests/bridle"          # vectors and scenarios headers, used by the host tests
VERSION_FILE="$REPO_ROOT/BRIDLE_VERSION"
REQUIREMENTS="$REPO_ROOT/scripts/requirements-dev.txt"

die() { echo "bridle-update: $*" >&2; exit 2; }

mode="update"
src=""
case "${1:-}" in
  --check) [[ $# -eq 1 ]] || die "usage: --check"; mode="check" ;;
  --dist) [[ $# -eq 2 ]] || die "usage: --dist DIR"; src="$2"; tag="" ;;
  --verify) [[ $# -eq 2 ]] || die "usage: --verify TAG"; mode="verify"; tag="$2"
    [[ "$tag" =~ ^bridle-v[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "a release tag is bridle-vX.Y.Z, not $tag" ;;
  bridle-v*) [[ $# -eq 1 ]] || die "usage: TAG"; tag="$1"
    [[ "$tag" =~ ^bridle-v[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "a release tag is bridle-vX.Y.Z, not $tag" ;;
  *) sed -n '2,14p' "$0" >&2; exit 2 ;;
esac

field() { sed -n "s/^$1=//p" "$VERSION_FILE"; }

if [[ "$mode" == "check" ]]; then
  status=0
  for pair in "bridle_enums.h:$ENUMS_DEST" "bridle_vectors.h:$TEST_DIR/bridle_vectors.h" "bridle_scenarios.h:$TEST_DIR/bridle_scenarios.h"; do
    name="${pair%%:*}"; path="$REPO_ROOT/${pair#*:}"
    want="$(sed -n "s/^$name sha256=//p" "$VERSION_FILE")"
    have="$(sha256sum "$path" | cut -d' ' -f1)"
    [[ -n "$want" && "$want" == "$have" ]] || { echo "bridle-check: ${pair#*:} differs from the $name BRIDLE_VERSION names; run scripts/bridle-update.sh" >&2; status=1; }
  done
  want_pin="bridle-protocol @ https://github.com/$BRIDLE_REPO/releases/download/$(field tag)/"
  # The release names the protocol it carries; the committed header says the same.
  header_proto="$(sed -n 's/.*PROTO_MAJOR = \([0-9]*\);.*/\1/p' "$REPO_ROOT/$ENUMS_DEST").$(sed -n 's/.*PROTO_MINOR = \([0-9]*\);.*/\1/p' "$REPO_ROOT/$ENUMS_DEST")"
  [[ "$(field protocol)" == "$header_proto" ]] \
    || { echo "bridle-check: BRIDLE_VERSION says protocol $(field protocol), the header $header_proto; run scripts/bridle-update.sh" >&2; status=1; }
  grep -q "^$want_pin.*#sha256=$(field wheel_sha256)\$" "$REQUIREMENTS" \
    || { echo "bridle-check: $REQUIREMENTS does not pin the wheel of $(field tag) named in BRIDLE_VERSION" >&2; status=1; }
  [[ $status -eq 0 ]] && echo "bridle-check: headers and wheel pin match $(field tag)"
  exit $status
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if [[ -n "$tag" ]]; then
  version="${tag#bridle-v}"
  src="$WORK/dl"
  mkdir -p "$src"
  files=(SHA256SUMS "bridle-cpp-$version.zip" "bridle_protocol-$version-py3-none-any.whl")
  # gh uses the caller's GitHub credentials, which a private repository needs; curl serves a public one.
  if command -v gh >/dev/null && gh release download "$tag" --repo "$BRIDLE_REPO" --dir "$src" \
       "${files[@]/#/--pattern=}" 2>/dev/null; then :
  else
    base="https://github.com/$BRIDLE_REPO/releases/download/$tag"
    for f in "${files[@]}"; do
      curl -sSLf -o "$src/$f" "$base/$f" || die "download of $base/$f failed (a private repository needs gh, logged in)"
    done
  fi
fi

(cd "$src" && sha256sum -c --quiet SHA256SUMS) || die "the files in $src do not match their SHA256SUMS"
# The two files are the ones SHA256SUMS lists, so a stray file in DIR is never taken.
zip="$src/$(awk '$2 ~ /^bridle-cpp-.*\.zip$/ {print $2}' "$src/SHA256SUMS")"
wheel="$src/$(awk '$2 ~ /^bridle_protocol-.*\.whl$/ {print $2}' "$src/SHA256SUMS")"
[[ -f "$zip" && -f "$wheel" ]] || die "SHA256SUMS in $src lists no zip and wheel"
unzip -q -o "$zip" -d "$WORK/headers"
[[ -n "$(sed -n 's/^protocol=//p' "$WORK/headers/BRIDLE_VERSION")" ]] || die "the zip names no protocol= (a release before 0.5.0 cannot be taken)"
release_tag="$(sed -n 's/^tag=//p' "$WORK/headers/BRIDLE_VERSION")"
[[ -z "$tag" || "$tag" == "$release_tag" ]] || die "the zip is for $release_tag, not $tag"
tag="$release_tag"
zip_sha="$(sha256sum "$zip" | cut -d' ' -f1)"; wheel_sha="$(sha256sum "$wheel" | cut -d' ' -f1)"
wheel_name="$(basename "$wheel")"
url="https://github.com/$BRIDLE_REPO/releases/download/$tag/$wheel_name"

if [[ "$mode" == "verify" ]]; then
  grep -q "^zip_sha256=$zip_sha\$" "$VERSION_FILE" || die "the release's zip differs from the one named in BRIDLE_VERSION"
  grep -q "^wheel_sha256=$wheel_sha\$" "$VERSION_FILE" || die "the release's wheel differs from the one named in BRIDLE_VERSION"
  grep -q "^bridle-protocol @ $url#sha256=$wheel_sha\$" "$REQUIREMENTS" || die "$REQUIREMENTS does not pin the release's wheel"
  echo "bridle-update: release $tag matches what is pinned here"
  exit 0
fi

# The commit the tag points at: read from the Bridle checkout (--dist) or from GitHub.
if [[ -d "$BRIDLE_CHECKOUT/.git" ]] && commit="$(git -C "$BRIDLE_CHECKOUT" rev-parse --verify -q "$tag^{commit}")"; then :
else
  commit="$(GIT_TERMINAL_PROMPT=0 git ls-remote "https://github.com/$BRIDLE_REPO.git" "refs/tags/$tag^{}" 2>/dev/null | cut -f1)" || true
  if [[ -z "$commit" ]] && command -v gh >/dev/null; then  # a private repository: with the caller's credentials
    commit="$(gh api "repos/$BRIDLE_REPO/commits/$tag" --jq .sha 2>/dev/null)" || commit=""
  fi
  [[ "$commit" =~ ^[0-9a-f]{40}$ ]] || die "cannot find the commit of $tag (no checkout at $BRIDLE_CHECKOUT, tag not on GitHub)"
fi

mkdir -p "$(dirname "$REPO_ROOT/$ENUMS_DEST")" "$REPO_ROOT/$TEST_DIR"
cp "$WORK/headers/bridle_enums.h" "$REPO_ROOT/$ENUMS_DEST"
cp "$WORK/headers/bridle_vectors.h" "$WORK/headers/bridle_scenarios.h" "$REPO_ROOT/$TEST_DIR/"
{
  cat "$WORK/headers/BRIDLE_VERSION"
  echo "commit=$commit"
  echo "zip_sha256=$zip_sha"
  echo "wheel_sha256=$wheel_sha"
} > "$VERSION_FILE"
# The pin replaces the wheel line and keeps the other requirements.
grep -v '^bridle-protocol @' "$REQUIREMENTS" > "$WORK/req" || true
echo "bridle-protocol @ $url#sha256=$wheel_sha" >> "$WORK/req"
cp "$WORK/req" "$REQUIREMENTS"
echo "bridle-update: $tag ($commit) taken; review and commit"
