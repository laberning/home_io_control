#!/usr/bin/env python3
"""Cross-source sync check for board pinouts documented in README.md / docs/home_io_control.md
against the single source of truth in ``config/boards/*.yaml``.

Board pinouts are the one duplicated dataset in this repo that is factual and safety-relevant: a
wrong ``clk_pin`` / ``cs_pin`` / ``dio1_pin`` does not fail a test, it produces a board that
silently does not transmit -- the exact failure mode the radio-robustness work spent weeks
distinguishing from protocol problems. ``config/boards/heltec-v2.yaml`` / ``heltec-v3.yaml`` /
``t3s3.yaml`` are now that dataset's home, consumed by every ``config/*.yaml`` via ``packages:``.
The docs still carry example copies for users to paste, and nothing else keeps those honest.

Same problem shape as ``scripts/check-yaml-emitters.py``: parse the source of truth, parse the
markdown copies with a regex, then diff.

Two kinds of markdown copy are checked:

* **Fenced ````yaml```` blocks** carrying an explicit ``<!-- board-pinout: <stem> -->`` marker on
  the line(s) immediately above the opening fence. The marker names the ``config/boards/`` file
  (by stem: ``heltec-v2`` / ``heltec-v3`` / ``t3s3``) the block must agree with. Blocks *without*
  a marker are left alone on purpose -- the docs also carry genuinely illustrative snippets and
  pinouts for boards this repo ships no package for (LilyGO T3-S3 SX1262/SX1276, T-Beam 1W, ...),
  and those must not be forced to match a shipped board. Resolving by ``radio_type`` alone cannot
  work: the README documents several correct boards per chip.
* **The README hardware table rows** for the three boards this repo ships a package for
  (see ``SHIPPED_TABLE_ROWS``). Those rows carry the full pin set inline in backticks and are the
  most-read copy in the repo. The other table rows -- untested boards, "any other ESP32" -- are
  skipped because there is no package to check them against.

Every pin/setting key a checked block or row names must match the resolved board file. Keys it
does not name are not required.

Run via ``make board-pinout-sync``; it is part of the ``lint`` composite target.
"""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BOARDS_DIR = REPO_ROOT / "config" / "boards"
DOC_FILES = [REPO_ROOT / "README.md", REPO_ROOT / "docs" / "home_io_control.md"]

# Keys this check compares. Pin keys resolve to an integer GPIO number; radio_type / tcxo_voltage
# / variant are compared as strings.
PIN_KEYS = {
    "clk_pin",
    "mosi_pin",
    "miso_pin",
    "cs_pin",
    "rst_pin",
    "dio0_pin",
    "dio1_pin",
    "dio4_pin",
    "busy_pin",
    "fem_en_pin",
    "vfem_pin",
    "fem_pa_pin",
}
STRING_KEYS = {"radio_type", "tcxo_voltage", "variant"}
ALL_KEYS = PIN_KEYS | STRING_KEYS

# Names of the config/boards/ files (stems). A block marker or a shipped table row resolves to one
# of these.
SHIPPED_BOARDS = {"heltec-v2", "heltec-v3", "t3s3"}

# README hardware-table rows for the three shipped boards, matched by a distinctive substring of
# the row's board-name cell. Ordered most-specific first so "LilyGO T3-S3 LR1121" is not shadowed
# by a looser "LilyGO T3-S3" match. Rows not listed here (untested boards, "Any other ESP32") have
# no package to check against and are skipped.
SHIPPED_TABLE_ROWS = [
    ("Heltec WiFi LoRa32 v2", "heltec-v2"),
    ("Heltec WiFi LoRa32 V3", "heltec-v3"),
    ("LilyGO T3-S3 LR1121", "t3s3"),
]

# <!-- board-pinout: heltec-v3 --> immediately above a fenced block.
BOARD_MARKER_RE = re.compile(r"<!--\s*board-pinout:\s*([a-z0-9-]+)\s*-->")
# `key: value` cell inside a markdown table row (or inline prose).
BACKTICK_PAIR_RE = re.compile(r"`([a-z0-9_]+):\s*([^`]+)`")


def _strip_comment(value: str) -> str:
    # Drop a trailing "  # ..." YAML comment; pin values never contain '#'.
    return value.split("#", 1)[0].strip()


def _coerce(key: str, value: str):
    value = value.strip().strip("'\"")
    if key in PIN_KEYS:
        try:
            return int(value, 0)
        except ValueError:
            return value
    return value


def _parse_pinout_text(text: str) -> dict:
    """Extract the ALL_KEYS this codebase cares about from a chunk of YAML text.

    Handles both ``key: 5`` and the expanded

        key:
          number: 5
          ignore_strapping_warning: true

    form, collapsing the latter to the ``number`` value (the only field this check compares).
    """
    result = {}
    lines = text.splitlines()
    for i, raw in enumerate(lines):
        m = re.match(r"^(\s*)([a-z0-9_]+):\s*(.*)$", raw)
        if not m:
            continue
        indent, key, value = m.group(1), m.group(2), _strip_comment(m.group(3))
        if key not in ALL_KEYS:
            continue
        if value == "":
            # Expanded mapping form — find the nested `number:` on a deeper indent.
            for follow in lines[i + 1 :]:
                fm = re.match(r"^(\s*)number:\s*(\S+)", follow)
                if fm and len(fm.group(1)) > len(indent):
                    value = _strip_comment(fm.group(2))
                    break
                if follow.strip() and not follow.startswith(indent + " "):
                    break
        if value == "":
            continue
        result[key] = _coerce(key, value)
    return result


def _parse_row_pairs(row_text: str) -> dict:
    """Extract the ALL_KEYS from the backticked `key: value` cells of one markdown table row."""
    result = {}
    for key, value in BACKTICK_PAIR_RE.findall(row_text):
        if key in ALL_KEYS:
            result[key] = _coerce(key, _strip_comment(value))
    return result


def load_board_files() -> dict:
    boards = {}
    for path in sorted(BOARDS_DIR.glob("*.yaml")):
        boards[path.stem] = _parse_pinout_text(path.read_text())
    return boards


def iter_marked_yaml_blocks(text: str):
    """Yield (line_number, board_stem, block_text) for every fenced ```yaml block that carries a
    ``<!-- board-pinout: <stem> -->`` marker in the two lines immediately above the fence."""
    lines = text.splitlines()
    for m in re.finditer(r"^```ya?ml[ \t]*\n(.*?)^```", text, re.MULTILINE | re.DOTALL):
        fence_line = text[: m.start()].count("\n")  # 0-based index of the ```yaml line
        marker = None
        for probe in (fence_line - 1, fence_line - 2):
            if 0 <= probe < len(lines):
                mm = BOARD_MARKER_RE.search(lines[probe])
                if mm:
                    marker = mm.group(1)
                    break
        if marker is not None:
            yield fence_line + 1, marker, m.group(1)


def iter_shipped_table_rows(text: str):
    """Yield (line_number, board_stem, row_text) for the README hardware-table rows of the three
    boards this repo ships a package for."""
    for lineno, raw in enumerate(text.splitlines(), start=1):
        if not raw.lstrip().startswith("|"):
            continue
        for needle, stem in SHIPPED_TABLE_ROWS:
            if needle in raw:
                yield lineno, stem, raw
                break


def _compare(found: dict, board_name: str, boards: dict, doc: Path, lineno: int) -> list:
    errors = []
    canonical = boards[board_name]
    for key, value in sorted(found.items()):
        if key == "variant":
            continue  # docs example blocks often omit or abbreviate the esp32 block
        if key not in canonical:
            errors.append(
                f"{doc.relative_to(REPO_ROOT)}:{lineno}: block for {board_name} sets "
                f"'{key}: {value}', but config/boards/{board_name}.yaml has no such key"
            )
        elif canonical[key] != value:
            errors.append(
                f"{doc.relative_to(REPO_ROOT)}:{lineno}: block for {board_name} sets "
                f"'{key}: {value}', but config/boards/{board_name}.yaml has "
                f"'{key}: {canonical[key]}'"
            )
    return errors


def main() -> int:
    boards = load_board_files()
    missing = SHIPPED_BOARDS - boards.keys()
    if missing:
        print(f"ERROR: config/boards/ is missing {sorted(missing)}", file=sys.stderr)
        return 1

    errors = []
    checked = 0
    for doc in DOC_FILES:
        text = doc.read_text()

        for lineno, stem, block in iter_marked_yaml_blocks(text):
            if stem not in boards:
                errors.append(
                    f"{doc.relative_to(REPO_ROOT)}:{lineno}: board-pinout marker names "
                    f"'{stem}', which is not a config/boards/*.yaml file"
                )
                continue
            found = _parse_pinout_text(block)
            pin_hits = [k for k in found if k in PIN_KEYS]
            if len(pin_hits) < 1:
                errors.append(
                    f"{doc.relative_to(REPO_ROOT)}:{lineno}: board-pinout marker for '{stem}' "
                    f"sits above a block that names no pin keys"
                )
                continue
            checked += 1
            errors.extend(_compare(found, stem, boards, doc, lineno))

        if doc.name == "README.md":
            for lineno, stem, row in iter_shipped_table_rows(text):
                found = _parse_row_pairs(row)
                if "radio_type" not in found:
                    continue
                checked += 1
                errors.extend(_compare(found, stem, boards, doc, lineno))

    if errors:
        print("Board pinout drift between docs and config/boards/:\n", file=sys.stderr)
        for e in errors:
            print(f"  {e}", file=sys.stderr)
        return 1

    print(f"board-pinout-sync: {checked} documented pinout block(s)/row(s) match config/boards/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
