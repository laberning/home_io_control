#!/usr/bin/env python3
"""Report how long receivers take to answer a request, from the golden-frame corpus.

A low-power request is a start frame this hub transmitted (`dir: tx`) whose CTRL1 has the low-power
bit (0x20) set; with `--mains` the requests are the start frames without it. Its reply is the next received frame sent from the request's destination to the
request's source, before a newer request for the same pair. Latency is `reply.t_ms - request.t_ms`.
The hub logs a transmitted frame when its transmission has ended, so this is the time an exchange
listens after its transmit, which is what the response window must cover. Passively overheard
requests (`dir: rx`) are left out: their stamp is the end of reception, and an overheard request is
often followed by another exchange's frames rather than its own reply.

The low-power result sizes LOW_POWER_RESPONSE_WAIT_MS and the mains result EXCHANGE_EARLY_RESPONSE_WAIT_MS
(proto_timing.h). Only tracked files are read.

Usage: scripts/corpus/reply_latency.py [--mains]   (prints the sample count, percentiles and per-capture counts)
"""

import math
import sys
from pathlib import Path

import yaml

import protolib

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
CAPTURES_DIR = REPO_ROOT / "tests" / "corpus" / "captures"

CTRL1_LOW_POWER = 0x20
NODE_ID_SIZE = 3
# Frame layout: CTRL0, CTRL1, destination (3), source (3), command.
CTRL1_OFFSET = 1
DST_OFFSET = 2
SRC_OFFSET = DST_OFFSET + NODE_ID_SIZE
HEADER_SIZE = SRC_OFFSET + NODE_ID_SIZE + 1


def latencies_in_capture(frames: list, low_power: bool = True) -> list:
    """Return the reply latencies (ms) of every answered start request in one capture's frames.

    The requests are the low-power ones (CTRL1 low-power bit set) or, with `low_power=False`, the
    ones without it.
    """
    pending: dict = {}  # (src, dst) -> t_ms of the newest unanswered start request of the wanted power class
    result = []
    for frame in frames:
        t_ms = frame.get("t_ms")
        raw = protolib.parse_hex(frame["hex"])
        if t_ms is None or len(raw) < HEADER_SIZE:
            continue
        dst = raw[DST_OFFSET:SRC_OFFSET]
        src = raw[SRC_OFFSET : SRC_OFFSET + NODE_ID_SIZE]
        if frame["dir"] == "tx" and protolib.is_start(raw[0]) and bool(raw[CTRL1_OFFSET] & CTRL1_LOW_POWER) == low_power:
            pending[(src, dst)] = t_ms
            continue
        request_t = pending.pop((dst, src), None) if frame["dir"] == "rx" else None  # a reply swaps the pair
        if request_t is not None and t_ms >= request_t:
            result.append(t_ms - request_t)
    return result


def percentile(sorted_values: list, fraction: float) -> int:
    """Nearest-rank percentile of an ascending, non-empty list."""
    return sorted_values[max(0, math.ceil(fraction * len(sorted_values)) - 1)]


def collect(captures_dir: Path, low_power: bool = True) -> dict:
    """Map capture id -> its reply latencies for the wanted power class, for captures that have any."""
    per_capture = {}
    for path in sorted(captures_dir.rglob("*.yaml")):
        data = yaml.safe_load(path.read_text(encoding="utf-8"))
        values = latencies_in_capture(data["frames"], low_power)
        if values:
            per_capture[data["id"]] = values
    return per_capture


def main(argv: list) -> int:
    low_power = "--mains" not in argv
    power_class = "low-power" if low_power else "mains"
    per_capture = collect(CAPTURES_DIR, low_power)
    values = sorted(v for samples in per_capture.values() for v in samples)
    if not values:
        print(f"reply_latency.py: no answered {power_class} request in the corpus", file=sys.stderr)
        return 1
    print(f"{power_class} replies: {len(values)} samples from {len(per_capture)} captures")
    for label, fraction in (("p50", 0.5), ("p90", 0.9), ("p95", 0.95), ("max", 1.0)):
        print(f"  {label}: {percentile(values, fraction)} ms")
    for window_ms in (150, 200, 400, 550):
        within = sum(1 for v in values if v <= window_ms)
        print(f"  within {window_ms} ms: {within} of {len(values)}")
    for capture_id, samples in per_capture.items():
        print(f"  {capture_id}: {len(samples)} samples, {sorted(samples)} ms")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
