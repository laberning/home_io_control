#!/usr/bin/env python3
"""Extract libFuzzer seed inputs from the golden-frame corpus.

Every `frames[].hex` byte string across `tests/corpus/captures/**/*.yaml` becomes one seed
file for `fuzz-frame` (`tests/fuzz/fuzz_frame_parse.cpp`). Two variants are written per frame
where `crc: present`: the as-captured bytes (what a driver would actually hand `parse()`) and
the CRC-stripped variant — cheap to produce and it costs nothing to seed both shapes.

Run via `make fuzz-frame` (not part of `make check` — fuzzing is time-boxed, on-demand work).
"""

import sys
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
from protolib import parse_hex  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
CAPTURES_DIR = REPO_ROOT / "tests" / "corpus" / "captures"
SEEDS_DIR = REPO_ROOT / "build" / "fuzz" / "seeds"


def main() -> int:
    if not CAPTURES_DIR.is_dir():
        print(f"error: captures directory not found: {CAPTURES_DIR}", file=sys.stderr)
        return 1

    SEEDS_DIR.mkdir(parents=True, exist_ok=True)
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
            raw = parse_hex(hex_str)

            seed_path = SEEDS_DIR / f"{capture_id}_{i}.bin"
            seed_path.write_bytes(raw)
            count += 1

            if frame.get("crc") == "present" and len(raw) > 2:
                stripped_path = SEEDS_DIR / f"{capture_id}_{i}_nocrc.bin"
                stripped_path.write_bytes(raw[:-2])
                count += 1

    print(f"Wrote {count} fuzz seed(s) to {SEEDS_DIR}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
