#!/usr/bin/env python3
"""Extract libFuzzer seed inputs from the golden-frame corpus.

Default mode: every `frames[].hex` byte string across `tests/corpus/captures/**/*.yaml` becomes
one seed file for `fuzz-frame` (`tests/fuzz/fuzz_frame_parse.cpp`). Two variants are written per
frame where `crc: present`: the as-captured bytes (what a driver would actually hand `parse()`)
and the CRC-stripped variant — cheap to produce and it costs nothing to seed both shapes.

`--soft-phy` mode: seeds for `fuzz-soft-phy` (`tests/fuzz/fuzz_soft_phy_rx.cpp`), which takes a
raw chip buffer rather than a frame. Every frame gets its CRC (recomputed, little-endian, the way
the soft PHY appends it before encoding) and is UART-encoded the way the soft PHY transmits it
(`protolib.uart_encode`), once as-is and once behind each of a few runs of idle-high bits, so the
probe's non-zero bit offsets are seeded too.

Run via `make fuzz-frame` / `make fuzz-soft-phy` (not part of `make check` — fuzzing is
time-boxed, on-demand work).
"""

import argparse
import sys
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
from protolib import crc_ccitt, parse_hex, uart_encode  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
CAPTURES_DIR = REPO_ROOT / "tests" / "corpus" / "captures"
SEEDS_DIR = REPO_ROOT / "build" / "fuzz" / "seeds"
SOFT_PHY_SEEDS_DIR = REPO_ROOT / "build" / "fuzz" / "seeds_soft_phy"

# Leading idle-high bits placed in front of a UART-encoded frame: 0 aligns the first cell at bit
# 0, the others land it at probe offsets 1, 4 and 9 (the last one UART_PROBE_MAX_BIT_OFFSET - 1).
SOFT_PHY_LEAD_BITS = (0, 1, 4, 9)


def shift_behind_idle_bits(encoded: bytes, lead_bits: int) -> bytes:
    """`encoded` delayed by `lead_bits` ones, padded with ones to a whole byte (line idle)."""
    if lead_bits == 0:
        return encoded
    value = int.from_bytes(encoded, "big")
    total_bits = len(encoded) * 8 + lead_bits
    pad_bits = -total_bits % 8
    ones_lead = ((1 << lead_bits) - 1) << (len(encoded) * 8)
    shifted = ((ones_lead | value) << pad_bits) | ((1 << pad_bits) - 1)
    return shifted.to_bytes((total_bits + pad_bits) // 8, "big")


def write_soft_phy_seeds(capture_id: str, index: int, frame: dict, raw: bytes) -> int:
    body = raw[:-2] if frame.get("crc") == "present" else raw
    crc = crc_ccitt(body)
    encoded = uart_encode(body + bytes([crc & 0xFF, crc >> 8]))
    for lead_bits in SOFT_PHY_LEAD_BITS:
        (SOFT_PHY_SEEDS_DIR / f"{capture_id}_{index}_lead{lead_bits}.bin").write_bytes(
            shift_behind_idle_bits(encoded, lead_bits)
        )
    return len(SOFT_PHY_LEAD_BITS)


def write_frame_seeds(capture_id: str, index: int, frame: dict, raw: bytes) -> int:
    (SEEDS_DIR / f"{capture_id}_{index}.bin").write_bytes(raw)
    if frame.get("crc") == "present" and len(raw) > 2:
        (SEEDS_DIR / f"{capture_id}_{index}_nocrc.bin").write_bytes(raw[:-2])
        return 2
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--soft-phy", action="store_true", help="write UART-encoded raw-buffer seeds instead")
    args = parser.parse_args()
    seeds_dir = SOFT_PHY_SEEDS_DIR if args.soft_phy else SEEDS_DIR
    write_seeds = write_soft_phy_seeds if args.soft_phy else write_frame_seeds

    if not CAPTURES_DIR.is_dir():
        print(f"error: captures directory not found: {CAPTURES_DIR}", file=sys.stderr)
        return 1

    seeds_dir.mkdir(parents=True, exist_ok=True)
    count = 0

    for path in sorted(CAPTURES_DIR.rglob("*.yaml")):
        with open(path, encoding="utf-8") as handle:
            data = yaml.safe_load(handle)
        if not data:
            continue

        capture_id = data.get("id", path.stem)
        for i, frame in enumerate(data.get("frames", [])):
            hex_str = frame.get("hex")
            if not hex_str:
                continue
            count += write_seeds(capture_id, i, frame, parse_hex(hex_str))

    print(f"Wrote {count} fuzz seed(s) to {seeds_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
