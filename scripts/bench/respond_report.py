#!/usr/bin/env python3
"""Summarise what `io2g4.py respond` printed about the EXECUTEs it was sent: for each one the wake train in
front of the request (long or short), the CTRL1 of the request and of the 0x3D, and the time from the end of
the stand-in's 0x3C to the start of the 0x3D. Used to read the answer delay of the Home IO Control hub through
the bridge off a responder's output.

    io2g4.py respond --port P --node BE0D01 --key-file key.txt | tee respond.log
    respond_report.py respond.log

Reads the lines `respond` prints (`heard 0xNN from X: CTRL1 0x.., after N wake frames` and `gap 0x3C end ->
0x3D start: N us`); anything else in the file is ignored. Exit status 1 when the log holds no EXECUTE.
"""

from __future__ import annotations

import argparse
import re
import statistics
import sys
from dataclasses import dataclass, field
from pathlib import Path

CMD_EXECUTE, CMD_CHALLENGE_RESP = 0x00, 0x3D
# A long train is 510 frames and the responder misses about 1 in 850 of them; a short one is 8 frames.
LONG_MIN_FRAMES, SHORT_MAX_FRAMES = 400, 8

HEARD = re.compile(r"^heard 0x([0-9a-f]{2}) from ([0-9A-F]{6}): CTRL1 0x([0-9a-f]{2}), after (\d+) wake frames$")
GAP = re.compile(r"^gap 0x3C end -> 0x3D start: (-?\d+) us$")


@dataclass
class Execute:
    """One EXECUTE as the stand-in heard it."""

    request_ctrl1: int  #: CTRL1 of the request
    wakes: int  #: wake frames heard before the request
    answer_ctrl1: int | None = None  #: CTRL1 of the 0x3D, None when none came
    gap_us: int | None = None  #: end of the 0x3C to the start of the 0x3D

    @property
    def train(self) -> str:
        if self.wakes >= LONG_MIN_FRAMES:
            return "long"
        return "short" if self.wakes <= SHORT_MAX_FRAMES else "other"


@dataclass
class Report:
    """The EXECUTEs of one log."""

    executes: list[Execute] = field(default_factory=list)

    @property
    def gaps(self) -> list[int]:
        return [e.gap_us for e in self.executes if e.gap_us is not None]


def parse(lines) -> Report:
    """The EXECUTEs in the lines of a responder's output."""
    report = Report()
    for line in lines:
        line = line.strip()
        if heard := HEARD.match(line):
            cmd, ctrl1, wakes = int(heard.group(1), 16), int(heard.group(3), 16), int(heard.group(4))
            if cmd == CMD_EXECUTE:
                report.executes.append(Execute(ctrl1, wakes))
            elif cmd == CMD_CHALLENGE_RESP and report.executes:
                report.executes[-1].answer_ctrl1 = ctrl1
        elif (gap := GAP.match(line)) and report.executes:
            report.executes[-1].gap_us = int(gap.group(1))
    return report


def render(report: Report) -> str:
    """The table and the summary."""
    rows = ["  # request CTRL1  wake frames  train   0x3D CTRL1  gap 0x3C end -> 0x3D start"]
    for n, e in enumerate(report.executes, 1):
        answer = "-" if e.answer_ctrl1 is None else f"0x{e.answer_ctrl1:02x}"
        gap = "-" if e.gap_us is None else f"{e.gap_us} us"
        rows.append(f"{n:3d}   0x{e.request_ctrl1:02x}      {e.wakes:5d}     {e.train:6s}  {answer:>10s}  {gap}")
    gaps = report.gaps
    rows.append("")
    rows.append(f"{len(report.executes)} EXECUTEs, {len(gaps)} answered with a 0x3D")
    if gaps:
        rows.append(f"gap: min {min(gaps)} us, median {statistics.median(gaps):.0f} us, max {max(gaps)} us (n = {len(gaps)})")
    trains = {name: sum(e.train == name for e in report.executes) for name in ("long", "short", "other")}
    rows.append("trains: " + ", ".join(f"{count} {name}" for name, count in trains.items()))
    ctrl1 = sorted({f"0x{e.answer_ctrl1:02x}" for e in report.executes if e.answer_ctrl1 is not None})
    rows.append("CTRL1 of the 0x3D: " + (", ".join(ctrl1) or "none"))
    return "\n".join(rows)


def main(argv: list[str], out=None) -> int:
    parser = argparse.ArgumentParser(prog="respond_report.py", description=__doc__.split("\n\n")[0])
    parser.add_argument("log", type=Path, help="what `io2g4.py respond` printed")
    args = parser.parse_args(argv[1:])
    report = parse(args.log.read_text(errors="replace").splitlines())
    print(render(report), file=out or sys.stdout)
    return 0 if report.executes else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
