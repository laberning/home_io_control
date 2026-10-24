#!/usr/bin/env python3
"""Turns a 2.4 GHz io-homecontrol sniffer log into a capture YAML (`phy: io_2g4`).

The input is the text log of an 802.15.4 sniffer (an nRF52840 running a sniffer sketch), one PSDU
per line, optionally preceded by the sniffer's µs timestamp:

    [<t_us>] <802.15.4 channel>: <PSDU hex, FCS included>

Lines starting with `#` and lines of `...` (frames the sender of the log left out) are skipped;
any other line is an error, so a mangled paste never loses a frame silently.

For each PSDU the ingester checks the FCS (CRC-16/KERMIT, protolib.crc_ccitt()), then:

- a wake frame (`00` + a 16-bit counter) is folded into the `wake_train` of the START frame it
  precedes, with the first and last counter seen and, when timed, their times. Counters must
  count down; missing ones show as `frames_captured` below the counter span, never hidden;
- an io frame has its `01 69 6F` header checked and stripped, and becomes a frame whose `hex` is
  the io frame alone (CTRL0 ... data, `crc: absent`: the io CRC is not on air at 2.4 GHz), with
  `freq` from the channel and `t_us` rebased to the log's first timestamp.

A Bridle trace of a co-processor listen session (e.g. Kite's `bench.py listen`, spec §12.1) is read with `--from-trace`: its
`rx` events with a good FCS become the same lines (the payload already holds the FCS; `t_us` marks the SFD
and has a 1 µs tick, which the capture records as `t_us_stamp`), and `--window FROM:TO` keeps the seconds
of the session, counted from the first frame of the trace (the first `rx` event, whatever its FCS, or with `--with-tx` the first frame sent, if earlier), that hold the wanted exchange.

`--with-tx` adds the frames the host commanded in that session (a bench tool's requests and answers): each `tx`
command is read with its `tx_done`, and a strobe in the command becomes the wake train in front of the frame, as the
command scheduled it (a capture of a controller's own train comes from a sniffer, not from this).

Direction is relative to one node, `--tx-node`: frames it sends are `tx`, all others `rx`. The
result is checked with validate.py (crypto aside, see below) before it is written.

A capture holding 2W crypto (0x3C/0x3D, a key transfer, a self-authenticated frame) is still under
its installation's real key, so it is written only outside the repo or to a git-ignored path;
`rekey_capture.py` then writes the committed, re-keyed file. A capture without crypto can go
straight into the corpus (the default output path, as for ingest.py):

    build/tools/venv/bin/python scripts/corpus/ingest_2g4.py sniffer.txt --tx-node 82E900 \\
        --id velux_klr200_statuspoll_example --device "VELUX KLR 200" --origin own-hardware --date 2026-09-27

Everything shared with ingest.py (provenance arguments, YAML rendering, expectation proposals)
comes from ingest.py; only the 802.15.4 framing is new here.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ingest  # noqa: E402
import protolib  # noqa: E402
import validate  # noqa: E402
from bridle import air, session  # noqa: E402
from naming import id_naming_problem  # noqa: E402

FCS_SIZE = 2
TRACE_CLOCK_WRAP_US = 1 << 32  # the co-processor's `t_us` is a u32 (spec §2)
# A trace stamps the SFD on a 1 MHz clock.
TRACE_STAMP = {"at": "sfd", "resolution_us": 1}
_LINE_RE = re.compile(r"^\s*(?:(?P<t_us>\d+)\s+)?(?P<channel>\d+):\s*(?P<hex>[0-9A-Fa-f]{2}(?:\s+[0-9A-Fa-f]{2})*)\s*$")


class SnifferLogError(Exception):
    """The log cannot be turned into a capture as it stands; the message names the line."""


@dataclass
class Psdu:
    """One sniffed 802.15.4 PSDU, FCS included, as the log gives it."""

    line_no: int
    channel: int
    psdu: bytes
    t_us: "int | None"

    @property
    def body(self) -> bytes:
        """The PSDU without its FCS."""
        return self.psdu[:-FCS_SIZE]

    def fcs_ok(self) -> bool:
        return len(self.psdu) > FCS_SIZE and int.from_bytes(self.psdu[-FCS_SIZE:], "little") == protolib.crc_ccitt(
            self.body)

    def wake_counter(self) -> "int | None":
        """The counter of a wake frame (`00` + 16-bit big-endian counter), None for any other PSDU."""
        body = self.body
        if len(body) == protolib.IO_2G4_WAKE_FRAME_LEN and body[0] == 0x00:
            return int.from_bytes(body[1:], "big")
        return None

    def io_frame(self) -> "bytes | None":
        """The io frame inside an io PSDU (header stripped), None for any other PSDU."""
        header = protolib.IO_2G4_PSDU_HEADER
        return self.body[len(header):] if self.body.startswith(header) else None


def parse_sniffer_log(text: str) -> "list[Psdu]":
    """Every PSDU line of a sniffer log, in order. Raises SnifferLogError on a line that is neither
    a PSDU, a `#` comment, a `...` gap marker nor blank, and on a log that mixes timed and untimed
    lines.
    """
    psdus = []
    for line_no, line in enumerate(text.splitlines(), start=1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#") or stripped == "...":
            continue
        match = _LINE_RE.match(line)
        if match is None:
            raise SnifferLogError(f"line {line_no}: not a sniffer PSDU line: {stripped!r}")
        t_us = int(match["t_us"]) if match["t_us"] is not None else None
        psdus.append(Psdu(line_no, int(match["channel"]), bytes.fromhex(match["hex"]), t_us))
    if len({p.t_us is None for p in psdus}) > 1:
        raise SnifferLogError("the log mixes timestamped and untimestamped lines")
    return psdus


def _tx_lines(command: dict, done: dict) -> "list[tuple[int, int, str]]":
    """The sniffer-log lines (time, channel, PSDU hex) of one transmission the host commanded, from its `tx`
    command and the co-processor's `tx_done`: the main frame, and the wake train in front of it when the command
    carried a strobe.

    The main frame is stamped at the end of its SFD (`t_main_start_us` plus the preamble and SFD). The train is
    the commanded one: frame i of `strobe_sent` starts `main_gap_ns + (count - 1 - i) x period_ns` before the main
    frame and carries the counter `count - 1 - i`, as the strobe template (`00` + a 16-bit counter) says. A
    transmission whose strobe frames were late has no such schedule, so it is refused.
    """
    channel = protolib.ieee802154_channel_of_freq_hz(command["freq_hz"])
    sync_us = air.SYNC_US
    main_start = done["t_main_start_us"]
    lines = []
    strobe = command.get("strobe")
    if strobe is not None:
        if done["strobe_late"]:
            raise SnifferLogError(f"a transmission of the trace had {done['strobe_late']} late strobe frames, so its "
                                  "train has no commanded schedule to read")
        count, period_ns, gap_ns = strobe["count"], strobe["period_ns"], strobe["main_gap_ns"]
        if strobe["template"] != "000000" or strobe["counter_offset"] != 1 or strobe["counter_width"] != 2:
            raise SnifferLogError("a strobe of the trace has a template other than `00` + a 16-bit counter")
        for i in range(done["strobe_sent"]):
            counter = count - 1 - i
            start_ns = -(gap_ns + (count - 1 - i) * period_ns)
            body = bytes([0x00]) + counter.to_bytes(2, "big")
            psdu = body + protolib.crc_ccitt(body).to_bytes(2, "little")
            lines.append((main_start + round(start_ns / 1000) + sync_us, channel, psdu.hex(" ").upper()))
    body = bytes.fromhex(command["payload"])
    psdu = body + protolib.crc_ccitt(body).to_bytes(2, "little")
    lines.append((main_start + sync_us, channel, psdu.hex(" ").upper()))
    return lines


def trace_to_sniffer_log(text: str, window_s: "tuple[float, float] | None" = None, with_tx: bool = False) -> str:
    """The sniffer-log lines (`<t_us> <channel>: <PSDU hex>`) for the received frames of a Bridle trace.

    Frames with a bad FCS are left out, as the ingester refuses them. `window_s` keeps the frames
    between its two times (seconds from the first frame of the trace, end excluded: the first received frame,
    whatever its FCS, or with `with_tx` the first frame the host sent, if that is earlier). With `with_tx`
    the transmissions the host commanded (a bench tool's requests and answers) are in the log too, in time
    order, read from each `tx` command and its `tx_done` (see _tx_lines()); without it only received frames
    are. Raises SnifferLogError when the trace holds no frame in the window.
    """
    messages = session.from_log(text)["messages"]
    pending = {m["seq"]: m["fields"] for m in messages if m["msg_type"] == "tx"}
    timed, previous, wraps = [], None, 0
    first_us = None  # the earliest frame of the trace, kept or not: what a window counts from

    def unwrapped(t_us: int) -> int:
        nonlocal previous, wraps
        if previous is not None and t_us < previous - TRACE_CLOCK_WRAP_US // 2:
            wraps += 1
        previous = t_us
        return t_us + wraps * TRACE_CLOCK_WRAP_US

    for message in messages:
        fields = message["fields"]
        if message["msg_type"] == "rx":
            t_us = unwrapped(fields["t_us"])
            first_us = t_us if first_us is None else min(first_us, t_us)
            if fields["crc"] == "ok":
                channel = protolib.ieee802154_channel_of_freq_hz(fields["freq_hz"])
                timed.append((t_us, channel, bytes.fromhex(fields["payload"]).hex(" ").upper()))
        elif with_tx and message["msg_type"] == "tx_done" and fields["cmd_seq"] in pending:
            offset = unwrapped(fields["t_main_start_us"]) - fields["t_main_start_us"]
            sent = [(t_us + offset, channel, psdu) for t_us, channel, psdu in
                    _tx_lines(pending.pop(fields["cmd_seq"]), fields)]
            first_us = min(([first_us] if first_us is not None else []) + [entry[0] for entry in sent])
            timed.extend(sent)
    timed.sort(key=lambda entry: entry[0])
    lines = [f"{t_us} {channel}: {psdu}" for t_us, channel, psdu in timed
             if window_s is None or window_s[0] <= (t_us - first_us) / 1e6 < window_s[1]]
    if not lines:
        raise SnifferLogError("the trace holds no received frame with a good FCS in that window")
    return "\n".join(lines) + "\n"


def parse_window(text: str) -> "tuple[float, float]":
    """`FROM:TO` in seconds, as `--window` takes it."""
    start, _, end = text.partition(":")
    try:
        window = (float(start), float(end))
    except ValueError:
        raise argparse.ArgumentTypeError(f"{text!r} is not FROM:TO in seconds") from None
    if window[0] >= window[1]:
        raise argparse.ArgumentTypeError(f"{text!r}: FROM must be below TO")
    return window


class SnifferFrame(protolib.RawFrame):
    """An io frame from the sniffer log, with its µs time and the wake train that preceded it."""

    def __init__(self, direction: str, io_frame: bytes, freq: int, t_us: "int | None", wake_train: "dict | None"):
        super().__init__(direction, io_frame.hex(" ").upper(), freq=freq)
        self.t_us = t_us
        self.wake_train = wake_train


def _wake_train(wakes: "list[Psdu]", before: Psdu) -> dict:
    counters = [w.wake_counter() for w in wakes]
    if any(later >= earlier for earlier, later in zip(counters, counters[1:])):
        raise SnifferLogError(f"line {wakes[0].line_no}: wake counters {counters} do not count down")
    if any(w.channel != before.channel for w in wakes):
        raise SnifferLogError(f"line {before.line_no}: the wake train before this frame changes channel")
    train = {"frames_captured": len(wakes), "first_counter": counters[0], "last_counter": counters[-1]}
    if before.t_us is not None:
        train.update(first_t_us=wakes[0].t_us, last_t_us=wakes[-1].t_us)
    return train


def fold_frames(psdus: "list[Psdu]", tx_node: bytes) -> "list[SnifferFrame]":
    """Check every FCS and header, fold wake trains onto the START frame they precede, and turn
    each io PSDU into a SnifferFrame. Times are rebased to the log's first timestamp.
    """
    base = min((p.t_us for p in psdus if p.t_us is not None), default=0)
    frames, wakes = [], []
    for psdu in psdus:
        if not psdu.fcs_ok():
            raise SnifferLogError(f"line {psdu.line_no}: FCS does not match the PSDU")
        if psdu.t_us is not None:
            psdu.t_us -= base
        if psdu.wake_counter() is not None:
            wakes.append(psdu)
            continue
        io_frame = psdu.io_frame()
        if io_frame is None or len(io_frame) < protolib.FRAME_MIN_SIZE:
            raise SnifferLogError(f"line {psdu.line_no}: neither a wake frame nor an io frame behind "
                                  f"{protolib.IO_2G4_PSDU_HEADER.hex(' ').upper()}")
        train = None
        if wakes:
            if not protolib.is_start(io_frame[0]):
                raise SnifferLogError(f"line {psdu.line_no}: a wake train precedes a frame without the START flag")
            train = _wake_train(wakes, psdu)
            wakes = []
        direction = "tx" if io_frame[5:8] == tx_node else "rx"
        freq = protolib.ieee802154_channel_freq_hz(psdu.channel)
        frames.append(SnifferFrame(direction, io_frame, freq, psdu.t_us, train))
    if wakes:
        raise SnifferLogError(f"line {wakes[0].line_no}: the log ends inside a wake train")
    return frames


def render_frame_yaml(frame: SnifferFrame) -> str:
    """ingest.py's frame rendering, plus `t_us` and `wake_train`."""
    lines = ingest.render_frame_yaml(frame).split("\n")
    extra = []
    if frame.t_us is not None:
        extra.append(f"    t_us: {frame.t_us}")
    if frame.wake_train is not None:
        fields = ", ".join(f"{key}: {value}" for key, value in frame.wake_train.items())
        extra.append(f"    wake_train: {{{fields}}}")
    return "\n".join(lines[:1] + extra + lines[1:])


def build_capture(args, text: str, stamp: "dict | None" = None) -> "tuple[str, list[SnifferFrame]]":
    """The capture YAML for a sniffer log, checked by validate.py except for crypto. `stamp` is the
    capture's `t_us_stamp` (what the log's times mark and their tick); None leaves it to the default."""
    tx_node = bytes.fromhex(args.tx_node)
    if len(tx_node) != 3:
        raise SnifferLogError(f"--tx-node {args.tx_node!r} is not a 3-byte node id")
    frames = fold_frames(parse_sniffer_log(text), tx_node)
    if not frames:
        raise SnifferLogError("no io frames in the log")
    header = protolib.IO_2G4_PSDU_HEADER.hex(" ").upper()
    capture_fields = ["phy: io_2g4", f'psdu_header: "{header}"']
    if stamp is not None:
        capture_fields.append(f"t_us_stamp: {{at: {stamp['at']}, resolution_us: {stamp['resolution_us']}}}")
    rendered = ingest.build_yaml(args, frames, "unknown", None,
                                 capture_fields=capture_fields,
                                 render_frame=render_frame_yaml)
    try:
        validate.validate_capture(yaml.safe_load(rendered), Path(args.id), check_crypto=False)
    except validate.ValidationError as exc:
        raise SnifferLogError(f"the capture fails validation: {exc}") from exc
    return rendered, frames


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ingest.add_capture_args(parser)
    parser.add_argument("--tx-node", required=True, metavar="HEX",
                        help="the node whose frames are `tx` (3-byte node id, e.g. 82E900); every other frame is `rx`")
    parser.add_argument("--from-trace", action="store_true",
                        help="the input is a Bridle trace of a co-processor listen session, not a sniffer log")
    parser.add_argument("--with-tx", action="store_true",
                        help="with --from-trace: the transmissions the host commanded (a bench tool's requests and "
                             "answers, with their commanded wake trains) are frames of the capture too")
    parser.add_argument("--window", type=parse_window, default=None, metavar="FROM:TO",
                        help="with --from-trace: keep the frames of these seconds, counted from the first frame of the trace (the first received frame, whatever its FCS; with --with-tx also the first sent, if earlier)")
    args = parser.parse_args()
    if (args.window or args.with_tx) and not args.from_trace:
        parser.error("--window and --with-tx need --from-trace")

    naming = id_naming_problem(args.id)
    if naming:
        parser.error(f"--id {args.id!r} {naming} — see tests/corpus/README.md 'Naming convention'")
    output_path = Path(args.output) if args.output else ingest.default_output_path(args.id)
    try:
        text = ingest.read_input(args.input)
        if args.from_trace:
            rendered, frames = build_capture(args, trace_to_sniffer_log(text, args.window, args.with_tx), TRACE_STAMP)
        else:
            rendered, frames = build_capture(args, text)
        if ingest.has_2w_crypto(frames) and ingest.git_could_commit(output_path):
            raise SnifferLogError(
                f"refusing to write {output_path}: the capture holds 2W crypto under its installation's real key, so "
                "write it outside the repo (or to a git-ignored path) and re-key it with rekey_capture.py")
    except SnifferLogError as exc:
        print(f"ingest_2g4.py: ABORT: {exc}", file=sys.stderr)
        return 1

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(rendered, encoding="utf-8")
    trains = sum(1 for f in frames if f.wake_train)
    print(f"ingest_2g4.py: wrote {output_path} ({len(frames)} io frames, {trains} wake train(s), key: unknown)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
