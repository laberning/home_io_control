#!/usr/bin/env python3
"""Drive a Bridle co-processor as a 2.4 GHz io-homecontrol controller, from a PC, with one board.

    io2g4.py poll    --port P --dst NODE --src NODE [--repeat N] [--hub-request] [--legacy-timing] [--freq HZ]
                     [--trace FILE]
    io2g4.py execute --port P --dst NODE (--open | --close | --stop | --position PCT)
                     [--src NODE] [--key-file FILE] [--tries N] [--answer-delay-ms MS] [--lp-on-response]
                     [--wake long|short] [--freq HZ] [--trace FILE]
    io2g4.py respond --port P --node NODE [--key-file FILE] [--seconds N] [--answer-window-ms MS] [--scan]
    (every command also takes --tx-power-dbm DBM)
                     [--state resting|moving] [--position PCT] [--travel-s SECONDS] [--freq HZ] [--trace FILE]

The commands send three kinds of frame between them, and nothing else: no pairing, no key transfer.

- `poll` sends the unauthenticated status read (command 0x03) that a KLF 200 sends, behind the long wake
  train, and listens on the same channel for the device's 0x04. It needs no key and never asks for one.
  It transmits with the node id given as `--src`: on a system with a KLF 200 that is the KLF 200's id,
  and the device answers it as it answers the KLF 200. `--hub-request` sends the Home IO Control hub's own
  status request instead of the KLF 200's (function 0x03, sub-index 0, a third byte 0; START, LOW_POWER):
  still an unauthenticated status read that moves nothing, but the one the hub will send.
- `execute` sends one authenticated EXECUTE (command 0x00) and, when the device challenges it (0x3C),
  the answer (0x3D). THIS MOVES THE ACTUATOR. The system key comes from a file (`--key-file`) or the
  environment variable IO_SYSTEM_KEY, never from the command line, and is never printed or written to a
  trace. The trace does hold the challenge and the answer, as every capture of an authenticated exchange
  does. The answer goes PC -> USB -> radio, so it is later than a controller's own; the command measures
  the time on air from the end of the challenge to the start of the answer and prints it.
  A try that ends in NO CHALLENGE or NO CONFIRMATION is repeated from the request after the hub's retry
  delay (`--tries`, default 3, as the hub); an ERROR from the device ends the run. The options below vary,
  one at a time, what the Home IO Control hub does differently from a VELUX controller on air:
  `--answer-delay-ms D` starts the answer D ms after the challenge ended, timed by the co-processor
  (`tx.t_main_us`; a D the PC cannot reach in time is sent at once, and the output says so);
  `--lp-on-response` sets CTRL1 LOW_POWER on the answer (VELUX sends it clear); `--wake short` puts the
  8-frame train in front of the request instead of the 510-frame one.
- `respond` stands in for a device on a second board: it answers a status read with a 0x04, challenges an
  EXECUTE with a random 0x3C and acknowledges a correct 0x3D with a 0x04. Each request gets the reply in the
  shape that request draws: the KLF 200's status read the 8 bytes a VELUX blind sent it, the hub's status
  read a reply in the layout of a 2.4 GHz blind's (stopped flag, target, current, remaining travel in
  seconds, last commander), reporting `--state` (default resting: stopped, target = current) at `--position`
  (default 50, 0 open to 100 closed). A moving stand-in sets off at its first status read and goes on from there:
  the position advances and the remaining seconds fall, at a full travel of `--travel-s` (default 72), and once it
  has arrived it answers as resting.
  It is for a bench with two boards and for the tests of this tool. It prints, for every status read, the time from the end of the request to the start of the reply (the bench's, not the blind's: a blind needs about 2.8 ms), and for every frame addressed to it, the wake frames that
  preceded it and its CTRL1, and for every 0x3D the time from the end of its 0x3C to the start of the 0x3D;
  `--answer-window-ms W` ignores a 0x3D that starts later than that, as a real device would.
  `--scan` listens on all three channels (the idle scan a 2.4 GHz host runs) and answers on the channel each
  request came in on, so a host's retries on another channel reach it; without it the stand-in hears `--freq` only.

Every command refuses a co-processor of another protocol version, and a long wait sends a `get_stats` at least every 25 s so
the co-processor's host link is not taken over (Bridle specification §2.1, §7).

Every command writes its `--trace` under a name that is free (`t6.trace`, then `t6-2.trace`, ...) and prints it.

Exit status: 0 when the device answered (a reply, DONE, or `respond` ending), 1 when it did not, 2 when it
challenged a status read, 3 when the bench itself failed (no board, a refused command, no key).

Frames and crypto are `scripts/corpus/protolib.py`'s, the commands and the framing the Bridle package's
(`bridle.conformance.Peer`, `bridle.benchkit`). `--exec CMD...` replaces `--port`: a program that
speaks Bridle on its stdin and stdout. Dependencies: the `bridle-protocol` wheel (PyYAML, pyserial for --port), cryptography.
"""

from __future__ import annotations

import argparse
import math
import os
import sys
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "corpus"))
import protolib  # noqa: E402
from bridle import air, benchkit, bridle, conformance  # noqa: E402

BenchError = benchkit.BenchError

# === What goes on the air ===

DEFAULT_SFD = 0x56
DEFAULT_EXECUTE_SRC = "C0FFEE"  # the bench's own node id (ref868.py's frames carry it too)
# The status read a KLF 200 sends (corpus capture velux_klf200_probe_status_poll_timed_2g4): function 0x01,
# then the 8 parameter bytes the KLF 200 always sends.
STATUS_REQUEST_DATA = bytes.fromhex("01 FE 01 01 01 01 01 01 01 00")
# Data of an EXECUTE (corpus capture velux_klr200_exchange_execute_open_2g4): originator 0x01, ACEI 0x63, the
# 2-byte main parameter, then `80 00 00 00`.
EXECUTE_ORIGINATOR_AND_ACEI = bytes.fromhex("0163")
EXECUTE_TRAILER = bytes.fromhex("80000000")
# The main parameter: the position in 1/512 of the way (0 open, 0xC800 closed), or the stop command.
MAIN_OPEN, MAIN_CLOSED, MAIN_STOP = 0x0000, 0xC800, 0xD200
MAIN_PER_PERCENT = 512
# What a responding device answers (corpus captures velux_klf200_probe_status_poll_timed_2g4 and
# velux_klr200_exchange_execute_open_2g4): the data of its 0x04.
RESPONDER_STATUS_DATA = bytes.fromhex("2D60000080000000")
RESPONDER_EXECUTE_DATA = bytes.fromhex("5460000080000000")
# The hub's own status request (proto_commands.cpp create_get_status): CMD_PRIVATE with function 0x03, sub-index 0
# and a third byte 0, behind START and LOW_POWER (corpus statuspoll captures: `4B 20 <dst> <src> 03 03 00 00`).
HUB_STATUS_REQUEST_DATA = bytes.fromhex("03 00 00")
# The data of the 0x04 a 2.4 GHz blind answers the hub's status read with (14 bytes): flags (bit 0 = stopped), a
# constant, target (u16), current (u16), the travel time still to go in seconds (u16 big endian), the last
# commander's node id (3 bytes), then `01 00 00`.
HUB_STATUS_STOPPED = 0x01
HUB_STATUS_RESTING_FLAGS, HUB_STATUS_MOVING_FLAGS = 0x05, 0x04  # as the blind's resting and travelling replies
HUB_STATUS_KIND = 0x60  # byte 1 of the status reply of the 2.4 GHz blinds, resting or travelling
HUB_STATUS_RESTING_REMAINING_S = 1  # what a blind at rest reports as remaining travel
DEFAULT_TRAVEL_S = 72.0  # the full travel of the blind the replies were measured on, open to closed
DEFAULT_RESPONDER_POSITION = 50.0
WAKE_FRAME_BYTES = 3  # the strobe template of the wake train (spec vector `tx_io_wake_strobe`)
# The long wake train before the status read, as the spec's vector gives it, and the values the spec had
# before it was measured, for an A/B against a device: `poll --legacy-timing`.
LEGACY_STROBE_VALUES = {"count": 512, "period_ns": 976563, "main_gap_ns": 1344000, "counter_unit_ns": 976563}

# === Timing of the bench ===

TX_TIMEOUT_S = 5.0  # a long train is ~0.5 s on air; the rest is the link's and the co-processor's latency
REPLY_WINDOW_S = 0.05  # how long after its tx_done a command waits for the device's frame
RESPONDER_WINDOW_S = 0.1  # how long a responder waits for the 0x3D it asked for
# The hub's gap between the tries of one command (proto_timing.h EXCHANGE_RETRY_DELAY_MS; a test pins the value).
RETRY_DELAY_S = 0.25
DEFAULT_TRIES = 3  # as the hub's exchange engine
# `tx.t_main_us` (spec §5) must start the main frame at least 1 ms after the command arrives; the link adds up to
# 1 ms per direction (spec §2.1). An answer asked for sooner than that after the challenge cannot be timed.
AT_TIME_MIN_US = 1000
LINK_LATENCY_US = 1000
MAX_DELAY_MS = 1000.0  # at_time reaches 1.1 s ahead; the rest is the link's and the PC's
POLL_GAP_S = 2.0  # between two status reads: a device that sleeps has gone back to sleep
KEY_ENV = "IO_SYSTEM_KEY"

EXIT_OK, EXIT_NO_ANSWER, EXIT_CHALLENGE, EXIT_BENCH = 0, 1, 2, 3


# === Frames ===

def build_frame(cmd: int, dst: bytes, src: bytes, data: bytes = b"", *, ctrl1: int = 0, start: bool = False,
                end: bool = False) -> bytes:
    """An io frame (CTRL0 ... data, no CRC): 2W, the length in CTRL0, START and END as asked."""
    length = protolib.FRAME_MIN_SIZE + len(data)
    if length > protolib.FRAME_MAX_SIZE:
        raise ValueError(f"an io frame is at most {protolib.FRAME_MAX_SIZE} bytes, not {length}")
    ctrl0 = (length - 1) | (protolib.CTRL0_START if start else 0) | (protolib.CTRL0_END if end else 0)
    return bytes([ctrl0, ctrl1]) + dst + src + bytes([cmd]) + data


def psdu_of(frame: bytes) -> bytes:
    """The 802.15.4 PSDU for an io frame, without the FCS the radio appends."""
    return protolib.IO_2G4_PSDU_HEADER + frame


def status_request(dst: bytes, src: bytes, hub: bool = False) -> bytes:
    """The status read: START, LOW_POWER, command 0x03; the KLF 200's, or with `hub` the hub's."""
    data = HUB_STATUS_REQUEST_DATA if hub else STATUS_REQUEST_DATA
    return build_frame(protolib.CMD_PRIVATE, dst, src, data, ctrl1=protolib.CTRL1_LOW_POWER, start=True)


def hub_status_data(*, stopped: bool, target_raw: int, current_raw: int, commander: bytes,
                    remaining_s: int = HUB_STATUS_RESTING_REMAINING_S) -> bytes:
    """The data of a 0x04 in the layout a 2.4 GHz blind answers the hub's status read with
    (raw positions: 0xC800 = closed; `remaining_s`: travel time still to go)."""
    flags = HUB_STATUS_RESTING_FLAGS if stopped else HUB_STATUS_MOVING_FLAGS
    return (bytes([flags, HUB_STATUS_KIND]) + target_raw.to_bytes(2, "big") + current_raw.to_bytes(2, "big")
            + remaining_s.to_bytes(2, "big") + commander + bytes([0x01, 0x00, 0x00]))


def responder_status_data(state: str, position_pct: float, commander: bytes, elapsed_s: float = 0.0,
                          travel_s: float = DEFAULT_TRAVEL_S) -> bytes:
    """What the stand-in reports to the hub's status read: `resting` is stopped with target = current at
    `position_pct`; `moving` is on its way from there to the limit farther away, `elapsed_s` after it set off
    and at the speed of a full travel in `travel_s`. The remaining seconds fall as the position advances, and
    the answer is the resting one once the target is reached."""
    start = round(position_pct * MAIN_PER_PERCENT)
    if state == "resting":
        return hub_status_data(stopped=True, target_raw=start, current_raw=start, commander=commander)
    target = MAIN_OPEN if position_pct >= 50 else MAIN_CLOSED
    rate = MAIN_CLOSED / travel_s  # raw units per second
    travelled = round(elapsed_s * rate)
    if travelled >= abs(target - start):
        return hub_status_data(stopped=True, target_raw=target, current_raw=target, commander=commander)
    current = start + travelled if target > start else start - travelled
    remaining_s = math.ceil(abs(target - current) / rate)
    return hub_status_data(stopped=False, target_raw=target, current_raw=current, commander=commander,
                           remaining_s=remaining_s)


def main_parameter(args: argparse.Namespace) -> int:
    """The EXECUTE's main parameter for `--open`, `--close`, `--stop` or `--position`."""
    if args.stop:
        return MAIN_STOP
    if args.position is not None:
        return round(args.position * MAIN_PER_PERCENT)
    return MAIN_CLOSED if args.close else MAIN_OPEN


def describe_move(args: argparse.Namespace) -> str:
    """`--open`, `--close`, `--stop` or `--position` in words."""
    if args.position is not None:
        return f"to position {args.position:g} (0 open, 100 closed)"
    return "stop" if args.stop else "close" if args.close else "open"


def execute_data(main: int) -> bytes:
    """The data of the EXECUTE: originator, ACEI, main parameter, trailer."""
    return EXECUTE_ORIGINATOR_AND_ACEI + main.to_bytes(2, "big") + EXECUTE_TRAILER


def execute_request(dst: bytes, src: bytes, main: int) -> bytes:
    """The EXECUTE: START, LOW_POWER, command 0x00."""
    return build_frame(protolib.CMD_EXECUTE, dst, src, execute_data(main), ctrl1=protolib.CTRL1_LOW_POWER, start=True)


def transcript_of(origin: bytes) -> bytes:
    """What the HMAC of a challenge covers: the command byte and the data of the frame that was challenged."""
    return origin[8:9] + origin[protolib.FRAME_MIN_SIZE:]


def challenge_response(dst: bytes, src: bytes, origin: bytes, challenge: bytes, key: bytes,
                       low_power: bool = False) -> bytes:
    """The 0x3D that answers `challenge` to the frame `origin`. CTRL1 is clear, as the VELUX controllers send it,
    unless `low_power` sets LOW_POWER, as the hub does."""
    return build_frame(protolib.CMD_CHALLENGE_RESP, dst, src,
                       protolib.create_hmac(transcript_of(origin), challenge, key),
                       ctrl1=protolib.CTRL1_LOW_POWER if low_power else 0)


@dataclass(frozen=True)
class Frame:
    """An io frame received over 2.4 GHz, with the co-processor's stamp and the level it arrived at."""

    raw: bytes  #: CTRL0 ... data, header and FCS removed
    t_us: int  #: co-processor clock at the end of the frame's SFD (spec §4)
    rssi_dbm: int
    air_bytes: int  #: PSDU length including the FCS
    heard_s: float = 0.0  #: the host's clock when the event was taken in

    @classmethod
    def from_rx(cls, fields: Mapping, heard_s: float = 0.0) -> "Frame | None":
        """The io frame of an `rx` event; None for a frame with a bad FCS, a wake frame or anything else."""
        payload = bytes.fromhex(fields["payload"])
        header, body = protolib.IO_2G4_PSDU_HEADER, payload[len(protolib.IO_2G4_PSDU_HEADER):-air.FCS_BYTES]
        if fields["crc"] != "ok" or not payload.startswith(header) or len(body) < protolib.FRAME_MIN_SIZE \
                or protolib.ctrl0_implied_length(body[0]) != len(body):
            return None
        return cls(body, fields["t_us"], fields["rssi_dbm"], len(payload), heard_s)

    @property
    def dst(self) -> bytes:
        return self.raw[2:5]

    @property
    def src(self) -> bytes:
        return self.raw[5:8]

    @property
    def cmd(self) -> int:
        return self.raw[8]

    @property
    def data(self) -> bytes:
        return self.raw[protolib.FRAME_MIN_SIZE:]

    @property
    def start_us(self) -> int:
        return (self.t_us - air.SYNC_US) % air.U32

    @property
    def end_us(self) -> int:
        """Co-processor clock at the frame's last bit."""
        return (self.t_us + (air.PHR_BYTES + self.air_bytes) * air.BYTE_US) % air.U32

    def hex(self) -> str:
        return self.raw.hex(" ").upper()


def gap_us(later_us: int, earlier_us: int) -> int:
    """`later_us - earlier_us` on the co-processor's wrapping u32 clock, signed."""
    delta = (later_us - earlier_us) % air.U32
    return delta - air.U32 if delta >= air.U32 // 2 else delta


# === Talking to the co-processor ===

class Events:
    """The events of a peer, in order, with those a transmission saw before its `tx_done` first.

    Looking for a frame skips what is not it, and counts the frames with a bad FCS it passes, so that a
    command can say "a frame was heard, but damaged" instead of "nothing"."""

    def __init__(self, peer: conformance.Peer, pending: list[conformance.Message] = ()):
        self.peer, self.pending, self.bad_fcs = peer, list(pending), 0

    def next(self, timeout_s: float) -> conformance.Message | None:
        benchkit.keep_alive(self.peer)  # a long wait must not lose the host link (spec §7)
        return self.pending.pop(0) if self.pending else self.peer.receive(timeout_s)

    def frame(self, timeout_s: float, wanted) -> Frame | None:
        """The next io frame for which `wanted(frame)` holds, waiting at most `timeout_s`."""
        deadline = self.peer.clock() + timeout_s
        while (message := self.next(max(deadline - self.peer.clock(), 0))) is not None:
            if message.name != "rx":
                continue
            self.bad_fcs += message.fields["crc"] == "bad"
            frame = Frame.from_rx(message.fields, message.at)
            if frame is not None and wanted(frame):
                return frame
        return None

    def idle(self, seconds: float) -> None:
        """Let `seconds` pass, taking in what arrives."""
        self.pending.clear()
        deadline = self.peer.clock() + seconds
        while self.peer.receive(max(deadline - self.peer.clock(), 0)) is not None:
            benchkit.keep_alive(self.peer)


def tx_fields(radio: dict, freq_hz: int, frame: bytes, strobe: dict | None = None,
              t_main_us: int | None = None) -> dict:
    """The `tx` command that sends `frame` (behind `strobe`, if given, starting at the co-processor time
    `t_main_us`, if given) and keeps receiving afterwards."""
    fields = {"radio": radio["radio"], "freq_hz": freq_hz, "then_rx": True, "payload": psdu_of(frame).hex()}
    if strobe is not None:
        fields["strobe"] = strobe
    if t_main_us is not None:
        fields["t_main_us"] = t_main_us
    return fields


def transmit(peer: conformance.Peer, radio: dict, freq_hz: int, frame: bytes, strobe: dict | None = None,
             t_main_us: int | None = None) -> tuple[conformance.Message, Events]:
    """Send `frame` (behind `strobe`, if given, at `t_main_us`, if given) with `then_rx`, and wait for its `tx_done`.

    Returns the `tx_done` and the events that arrived before it. Raises BenchError when the co-processor
    refuses the transmission or does not finish it."""
    seq = peer.send("tx", tx_fields(radio, freq_hz, frame, strobe, t_main_us))
    deadline, before = peer.clock() + TX_TIMEOUT_S, []
    while (message := peer.receive(max(deadline - peer.clock(), 0))) is not None:
        if message.fields.get("cmd_seq") != seq:
            before.append(message)
        elif message.name == "tx_done":
            return message, Events(peer, before)
        elif message.name == "error":
            raise BenchError(f"the co-processor refused the transmission: {message.fields['code']} "
                             f"(detail {message.fields['detail']})")
    raise BenchError(f"no tx_done within {TX_TIMEOUT_S:.0f} s of the transmission")


def wake_strobe(legacy: bool, short: bool = False) -> dict:
    """The strobe of the long wake train: the spec's vector `tx_io_wake_strobe`, or with `legacy` the values
    the spec had before they were measured; with `short`, the 8-frame train of `tx_io_short_wake_strobe`."""
    if short:
        return dict(conformance._vector("tx_io_short_wake_strobe")["strobe"])
    strobe = conformance._vector("tx_io_wake_strobe")["strobe"]
    return {**strobe, **LEGACY_STROBE_VALUES} if legacy else dict(strobe)


def answer_time(delay_ms: float, challenge: Frame, now_s: float) -> tuple[int | None, str]:
    """The co-processor time `tx.t_main_us` that starts an answer `delay_ms` after `challenge` ended, and "".

    The PC knows the challenge's end on the co-processor's clock, but not when its next command will arrive
    there: the event reached the PC up to one link latency after the end, the command takes one more, and the
    co-processor wants the main frame at least `AT_TIME_MIN_US` after it arrives. A delay shorter than that
    cannot be timed: (None, note) says so, and the caller sends at once."""
    wanted_us = round(delay_ms * 1000)
    reach_us = round((now_s - challenge.heard_s) * 1e6) + 2 * LINK_LATENCY_US + AT_TIME_MIN_US
    if wanted_us < reach_us:
        return None, (f"--answer-delay-ms {delay_ms:g} is BELOW what this link can time (it cannot start an answer "
                      f"sooner than about {reach_us / 1000:.1f} ms after the challenge ended): the answer WAS SENT AT ONCE, "
                      f"not after {delay_ms:g} ms")
    return (challenge.end_us + wanted_us) % air.U32, ""


def hexs(data: bytes) -> str:
    return data.hex(" ").upper()


def from_device(dst: bytes, src: bytes):
    """The test for a frame that the device `dst` sent to the controller `src`."""
    return lambda frame: frame.src == dst and frame.dst == src


# === poll ===

def poll(peer: conformance.Peer, radio: dict, args: argparse.Namespace, out) -> int:
    """One or more status reads; exit status 0 if any was answered with a 0x04."""
    request = status_request(args.dst, args.src, hub=args.hub_request)
    strobe = wake_strobe(args.legacy_timing)
    answered = 0
    for attempt in range(1, args.repeat + 1):
        label = f"try {attempt}/{args.repeat}"
        done, events = transmit(peer, radio, args.freq, request, strobe)
        late = f", {done.fields['strobe_late']} late" if done.fields["strobe_late"] else ""
        print(f"{label}: sent {hexs(request)} behind {done.fields['strobe_sent']} wake frames{late}", file=out)
        reply = events.frame(REPLY_WINDOW_S, from_device(args.dst, args.src))
        end = done.fields["t_main_end_us"]
        if reply is not None and reply.cmd == protolib.CMD_CHALLENGE_REQ:
            print(f"{label}: CHALLENGE {reply.hex()} - the device wants authentication; nothing more is sent", file=out)
            return EXIT_CHALLENGE
        if reply is not None and reply.cmd == protolib.CMD_PRIVATE_RESP:
            answered += 1
            print(f"{label}: REPLY {reply.hex()} at {reply.rssi_dbm} dBm, "
                  f"{gap_us(reply.start_us, end)} us after the request ended", file=out)
        elif reply is not None:
            print(f"{label}: OTHER FRAME from the device, command {reply.cmd:#04x}: {reply.hex()}", file=out)
        else:
            print(f"{label}: NO REPLY within {REPLY_WINDOW_S * 1000:.0f} ms{_damaged(events)}", file=out)
        if attempt < args.repeat:
            events.idle(POLL_GAP_S)
    print(f"{answered} of {args.repeat} status reads answered", file=out)
    return EXIT_OK if answered else EXIT_NO_ANSWER


def _damaged(events: Events) -> str:
    return f" ({events.bad_fcs} frame(s) with a bad FCS heard)" if events.bad_fcs else ""


# === execute ===

def execute(peer: conformance.Peer, radio: dict, args: argparse.Namespace, key: bytes, out) -> int:
    """One authenticated EXECUTE, tried up to `args.tries` times as the hub does. Exit status 0 on DONE, else
    that of the last try."""
    print(f"moving the device {args.dst.hex().upper()}: {describe_move(args)}", file=out)
    code = EXIT_NO_ANSWER
    for attempt in range(1, args.tries + 1):
        code, retry = execute_once(peer, radio, args, key, out, f"try {attempt}/{args.tries}")
        if not retry or attempt == args.tries:
            break
        Events(peer).idle(RETRY_DELAY_S)
    return code


def execute_once(peer: conformance.Peer, radio: dict, args: argparse.Namespace, key: bytes, out,
                 label: str) -> tuple[int, bool]:
    """One request with its challenge and answer. Returns the exit status and whether another try may follow:
    a silent device may come round, a device that answered with an ERROR will not."""
    request = execute_request(args.dst, args.src, main_parameter(args))
    strobe = wake_strobe(legacy=False, short=args.wake == "short")
    done, events = transmit(peer, radio, args.freq, request, strobe)
    print(f"{label}: sent {hexs(request)} behind {done.fields['strobe_sent']} wake frames ({args.wake} train)", file=out)
    from_dst = from_device(args.dst, args.src)
    challenge = events.frame(REPLY_WINDOW_S, lambda f: from_dst(f) and f.cmd in (
        protolib.CMD_CHALLENGE_REQ, protolib.CMD_ERROR_RESP))
    if challenge is None:
        print(f"{label}: NO CHALLENGE within {REPLY_WINDOW_S * 1000:.0f} ms: the device did not accept the request "
              f"(wake train, address or source){_damaged(events)}", file=out)
        return EXIT_NO_ANSWER, True
    if challenge.cmd == protolib.CMD_ERROR_RESP:
        print(f"{label}: ERROR {challenge.hex()}: the device refused the request", file=out)
        return EXIT_NO_ANSWER, False
    answer = challenge_response(args.dst, args.src, request, challenge.data[:protolib.HMAC_SIZE], key,
                                low_power=args.lp_on_response)
    t_main_us = None
    if args.answer_delay_ms is not None:
        t_main_us, note = answer_time(args.answer_delay_ms, challenge, peer.clock())
        if note:
            print(f"{label}: {note}", file=out)
    answered, after = transmit(peer, radio, args.freq, answer, t_main_us=t_main_us)
    delay_us = gap_us(answered.fields["t_main_start_us"], challenge.end_us)
    if args.answer_delay_ms is None:
        asked = ""
    elif t_main_us is None:
        asked = f"; asked for {args.answer_delay_ms:g} ms, sent at once (below the floor of this link)"
    else:
        asked = f"; asked for {args.answer_delay_ms:g} ms"
    print(f"{label}: challenge {challenge.hex()} at {challenge.rssi_dbm} dBm; answered {hexs(answer)}, "
          f"{delay_us} us after the challenge ended (on air){asked}", file=out)
    confirmation = after.frame(REPLY_WINDOW_S, lambda f: from_dst(f) and f.cmd in (
        protolib.CMD_PRIVATE_RESP, protolib.CMD_ERROR_RESP))
    if confirmation is None:
        print(f"{label}: NO CONFIRMATION within {REPLY_WINDOW_S * 1000:.0f} ms of the answer, sent {delay_us} us after "
              f"the challenge: the key is wrong, or the answer was too late{_damaged(after)}", file=out)
        return EXIT_NO_ANSWER, True
    if confirmation.cmd == protolib.CMD_ERROR_RESP:
        print(f"{label}: ERROR {confirmation.hex()}: the device refused the answer", file=out)
        return EXIT_NO_ANSWER, False
    print(f"{label}: DONE {confirmation.hex()}", file=out)
    return EXIT_OK, False


# === respond ===

def is_wake_frame(fields: Mapping) -> bool:
    """Whether an `rx` event is a wake-train frame: a good frame of the strobe template's size (3 bytes and the
    FCS), which is shorter than any io frame."""
    return fields["crc"] == "ok" and len(bytes.fromhex(fields["payload"])) == WAKE_FRAME_BYTES + air.FCS_BYTES


# The idle scan a 2.4 GHz host runs (spec vector `rx_scan_io_idle`): all three channels, 1.5 ms each, held 8 ms on
# a frame start.
SCAN_FREQS_HZ = (bridle.CH15, bridle.CH20, bridle.CH25)
SCAN_DWELL_US, SCAN_HOLD_US = 1500, 8000
# How long a scanning stand-in stays on the channel of its last transmission before it scans again: long enough for
# the rest of an exchange (the 0x3D comes a few ms after the 0x3C), short against the hub's retry gap.
RESCAN_AFTER_S = 0.1


class Responder:
    """A stand-in for a device with the node id `node` and the system key `key`.

    With `scan`, it listens on all three channels as a real device does (Kite's `rx_scan`) and answers on the
    channel the request came in on, so a host's retries on another channel reach it; after its own transmission it
    stays on that channel for RESCAN_AFTER_S, then scans again. Without, it listens and answers on `freq_hz` only.

    Answers a status read with a 0x04; challenges an EXECUTE with a random 0x3C and acknowledges a correct
    0x3D with a 0x04, and stays silent for a wrong one. With `answer_window_ms`, a 0x3D that starts later than
    that after the end of the 0x3C is ignored, as a device that has given up on its challenge ignores it.
    `step()` takes in what has arrived without waiting, so a test can run it inside a simulated clock."""

    def __init__(self, peer: conformance.Peer, radio: dict, freq_hz: int, node: bytes, key: bytes, out,
                 random_bytes=os.urandom, answer_window_ms: float | None = None, state: str = "resting",
                 position_pct: float = DEFAULT_RESPONDER_POSITION, travel_s: float = DEFAULT_TRAVEL_S,
                 scan: bool = False):
        self.peer, self.radio, self.freq_hz, self.node, self.key, self.out = peer, radio, freq_hz, node, key, out
        self.scan = scan
        self.reply_freq_hz = freq_hz  # the channel the last request to us came in on (scan) or `freq_hz`
        self.rescan_at: float | None = None  # the host's clock when a scanning stand-in resumes its scan
        self.random_bytes, self.answer_window_ms = random_bytes, answer_window_ms
        self.state, self.position_pct = state, position_pct  # what the hub's status reads are answered with
        self.travel_s = travel_s  # a full travel, for a moving state
        self.moving_since: float | None = None  # the host's clock at the first status read answered as moving
        self.status_seq: dict[int, int] = {}  # cmd_seq of a status reply we sent -> the end of the read it answers
        self.open: dict[bytes, tuple[bytes, bytes]] = {}  # controller -> (the EXECUTE it sent, our challenge)
        self.challenge_seq: dict[int, bytes] = {}  # cmd_seq of a 0x3C we sent -> the controller it went to
        self.challenge_end: dict[bytes, int] = {}  # controller -> co-processor time our 0x3C ended
        self.wakes = 0  # wake frames heard since the last io frame

    def start_scan(self) -> None:
        """Scan the three channels (a scanning stand-in only); the ack is not waited for."""
        self.rescan_at = None
        self.peer.send("rx_scan", {"radio": self.radio["radio"], "dwell_us": SCAN_DWELL_US, "hold_us": SCAN_HOLD_US,
                                   "freqs_hz": list(SCAN_FREQS_HZ)})

    def step(self, wait_s: float = 0.0) -> None:
        """Answer what has arrived, after waiting at most `wait_s` for something to."""
        benchkit.keep_alive(self.peer)  # a long listen must not lose the host link (spec §7)
        if self.scan and self.rescan_at is not None and self.peer.clock() >= self.rescan_at:
            self.start_scan()
        first = self.peer.receive(wait_s) if wait_s else None
        for message in ([first] if first else []) + self.peer.take():
            if message.name == "tx_done" and message.fields["cmd_seq"] in self.challenge_seq:
                controller = self.challenge_seq.pop(message.fields["cmd_seq"])
                self.challenge_end[controller] = message.fields["t_main_end_us"]
                continue
            if message.name == "tx_done" and message.fields["cmd_seq"] in self.status_seq:
                latency = gap_us(message.fields["t_main_start_us"], self.status_seq.pop(message.fields["cmd_seq"]))
                print(f"status reply latency, end of the request -> start of the reply: {latency} us", file=self.out)
                continue
            if message.name != "rx":
                continue
            self.wakes += is_wake_frame(message.fields)
            frame = Frame.from_rx(message.fields, message.at)
            if frame is None:
                continue
            wakes, self.wakes = self.wakes, 0
            if frame.dst == self.node:
                channel = ""
                if self.scan:
                    self.reply_freq_hz = message.fields.get("freq_hz", self.freq_hz)
                    channel = f" on {self.reply_freq_hz} Hz"
                print(f"heard {frame.cmd:#04x} from {frame.src.hex().upper()}{channel}: CTRL1 {frame.raw[1]:#04x}, "
                      f"after {wakes} wake frames", file=self.out)
                self.handle(frame)

    def handle(self, frame: Frame) -> None:
        controller = frame.src
        if frame.cmd == protolib.CMD_PRIVATE:
            hub_shaped = frame.data == HUB_STATUS_REQUEST_DATA
            data = self.status_data(controller) if hub_shaped else RESPONDER_STATUS_DATA
            self.status_seq[self.send(protolib.CMD_PRIVATE_RESP, controller, data, end=True)] = frame.end_us
            shape = f" in the hub's layout, {self.state} at {self.position_pct:g}" if hub_shaped else ""
            print(f"status read from {controller.hex().upper()}: answered 0x04{shape}", file=self.out)
        elif frame.cmd == protolib.CMD_EXECUTE:
            challenge = self.random_bytes(protolib.HMAC_SIZE)
            self.open[controller] = (frame.raw, challenge)
            self.challenge_end.pop(controller, None)
            self.challenge_seq[self.send(protolib.CMD_CHALLENGE_REQ, controller, challenge)] = controller
            print(f"EXECUTE from {controller.hex().upper()}: challenged", file=self.out)
        elif frame.cmd == protolib.CMD_CHALLENGE_RESP and controller in self.open:
            origin, challenge = self.open.pop(controller)
            who = controller.hex().upper()
            if controller in self.challenge_end:
                gap = gap_us(frame.start_us, self.challenge_end[controller])
                print(f"gap 0x3C end -> 0x3D start: {gap} us", file=self.out)
                if self.answer_window_ms is not None and gap > self.answer_window_ms * 1000:
                    print(f"answer from {who} came {gap} us after the challenge, past the window of "
                          f"{self.answer_window_ms:g} ms: ignored", file=self.out)
                    return
            if protolib.verify_hmac(transcript_of(origin), frame.data[:protolib.HMAC_SIZE], challenge, self.key):
                self.send(protolib.CMD_PRIVATE_RESP, controller, RESPONDER_EXECUTE_DATA, end=True)
                print(f"answer from {who} is right: moved, answered 0x04", file=self.out)
            else:
                print(f"answer from {who} is wrong: no reply", file=self.out)

    def status_data(self, controller: bytes) -> bytes:
        """The answer to the hub's status read now: a moving state starts at the first read and goes on from there."""
        now = self.peer.clock()
        if self.state == "moving" and self.moving_since is None:
            self.moving_since = now
        elapsed = 0.0 if self.moving_since is None else now - self.moving_since
        return responder_status_data(self.state, self.position_pct, controller, elapsed, self.travel_s)

    def send(self, cmd: int, dst: bytes, data: bytes, end: bool = False) -> int:
        """Transmit a frame without waiting for it (`step()` goes on taking in events); returns the command's seq."""
        frame = build_frame(cmd, dst, self.node, data, end=end)
        if self.scan:
            self.rescan_at = self.peer.clock() + RESCAN_AFTER_S
        return self.peer.send("tx", tx_fields(self.radio, self.reply_freq_hz, frame))

    def run(self, seconds: float) -> None:
        """Serve until `seconds` have passed (0: until Ctrl-C)."""
        deadline = self.peer.clock() + seconds if seconds else None
        if self.scan:
            self.start_scan()
        try:
            while deadline is None or self.peer.clock() < deadline:
                self.step(0.05)
        except KeyboardInterrupt:
            pass


# === Command line ===

def node_id(text: str) -> bytes:
    """A node id: 6 hex digits."""
    try:
        node = bytes.fromhex(text)
    except ValueError:
        node = b""
    if len(node) != 3:
        raise argparse.ArgumentTypeError(f"{text!r} is not a node id of 6 hex digits")
    return node


def milliseconds(text: str) -> float:
    """A time in ms, 0 to `MAX_DELAY_MS`."""
    try:
        value = float(text)
    except ValueError:
        value = -1.0
    if not 0.0 <= value <= MAX_DELAY_MS:
        raise argparse.ArgumentTypeError(f"{text!r} is not a time between 0 and {MAX_DELAY_MS:g} ms")
    return value


def percent(text: str) -> float:
    """A position in percent, 0 to 100."""
    try:
        value = float(text)
    except ValueError:
        value = -1.0
    if not 0.0 <= value <= 100.0:
        raise argparse.ArgumentTypeError(f"{text!r} is not a position between 0 and 100")
    return value


def positive_float(text: str) -> float:
    """A number greater than 0."""
    try:
        value = float(text)
    except ValueError:
        value = 0.0
    if not value > 0.0:
        raise argparse.ArgumentTypeError(f"{text!r} is not a number greater than 0")
    return value


def positive_int(text: str) -> int:
    """A whole number of at least 1."""
    if not text.isdigit() or int(text) < 1:
        raise argparse.ArgumentTypeError(f"{text!r} is not a whole number of at least 1")
    return int(text)


def load_key(key_file: Path | None, environ: Mapping[str, str]) -> bytes:
    """The system key from `key_file`, else from IO_SYSTEM_KEY: 32 hex digits, whitespace ignored.

    No message here names the key, the path or the variable's value: the path may be the key typed
    where a path belongs."""
    if key_file is not None:
        try:
            text = key_file.read_text()
        except OSError:
            raise BenchError("cannot read the key file") from None
    else:
        text = environ.get(KEY_ENV)
        if text is None:
            raise BenchError(f"no system key: put it in a file and give --key-file, or set {KEY_ENV}")
    digits = "".join(text.split())
    try:
        key = bytes.fromhex(digits)
    except ValueError:
        key = b""
    if len(key) != protolib.AES_KEY_SIZE:
        raise BenchError(f"the system key must be {2 * protolib.AES_KEY_SIZE} hex digits")
    return key


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="io2g4.py", description=__doc__.split("\n\n")[0], allow_abbrev=False)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("poll", "execute", "respond"):
        p = sub.add_parser(name, allow_abbrev=False)
        benchkit.add_link_arguments(p)
        p.add_argument("--freq", type=int, default=bridle.CH20, help="Hz, a channel centre (default channel 20)")
        p.add_argument("--sfd", type=lambda s: int(s, 0), default=DEFAULT_SFD, help="start-of-frame delimiter (default 0x56)")
        p.add_argument("--key", dest="refused_key", help=argparse.SUPPRESS)  # refused, see main()
        p.add_argument("--tx-power-dbm", type=int, metavar="DBM",
                       help="the radio's TX power, set after the spec vector's (default: the vector's 8 dBm); a "
                            "lower value stands in for a distant device without moving the board")
    poll_parser, execute_parser, respond_parser = sub.choices["poll"], sub.choices["execute"], sub.choices["respond"]
    poll_parser.add_argument("--dst", type=node_id, required=True, help="the device's node id (6 hex digits)")
    poll_parser.add_argument("--src", type=node_id, required=True,
                             help="the controller id the request carries (the KLF 200's, on its system)")
    poll_parser.add_argument("--repeat", type=positive_int, default=3, help=f"how many status reads, {POLL_GAP_S:g} s apart (default 3)")
    poll_parser.add_argument("--hub-request", action="store_true",
                             help="send the Home IO Control hub's own status request (03 03 00 00) instead of the KLF 200's")
    poll_parser.add_argument("--legacy-timing", action="store_true",
                             help="the wake train with 512 frames, 976563 ns and 1344 us, to compare with the measured one")
    execute_parser.add_argument("--dst", type=node_id, required=True, help="the device's node id (6 hex digits)")
    execute_parser.add_argument("--src", type=node_id, default=node_id(DEFAULT_EXECUTE_SRC),
                                help=f"the controller id the request carries (default {DEFAULT_EXECUTE_SRC})")
    move = execute_parser.add_mutually_exclusive_group(required=True)
    move.add_argument("--open", action="store_true")
    move.add_argument("--close", action="store_true")
    move.add_argument("--stop", action="store_true")
    move.add_argument("--position", type=percent, metavar="PCT", help="io scale: 0 open, 100 closed")
    execute_parser.add_argument("--tries", type=positive_int, default=DEFAULT_TRIES,
                                help=f"how many times a silent device is asked again, {RETRY_DELAY_S * 1000:.0f} ms apart, "
                                     f"as the hub does (default {DEFAULT_TRIES})")
    execute_parser.add_argument("--answer-delay-ms", type=milliseconds, metavar="MS",
                                help="start the answer to the challenge this long after the challenge ended, on air "
                                     "(default: as soon as the PC can)")
    execute_parser.add_argument("--lp-on-response", action="store_true",
                                help="set CTRL1 LOW_POWER on the answer, as the hub does (VELUX sends it clear)")
    execute_parser.add_argument("--wake", choices=("long", "short"), default="long",
                                help="the train in front of the request: the 510-frame one a VELUX controller sends, "
                                     "or the 8-frame one (default long)")
    respond_parser.add_argument("--node", type=node_id, required=True, help="the node id to answer as")
    respond_parser.add_argument("--state", choices=("resting", "moving"), default="resting",
                                help="what the stand-in tells the hub's status reads (default resting: stopped, target = current)")
    respond_parser.add_argument("--position", type=percent, default=DEFAULT_RESPONDER_POSITION, metavar="PCT",
                                help=f"where the stand-in is, io scale: 0 open, 100 closed (default {DEFAULT_RESPONDER_POSITION:g})")
    respond_parser.add_argument("--travel-s", type=positive_float, default=DEFAULT_TRAVEL_S, metavar="SECONDS",
                                help=f"how long a full travel takes, for --state moving (default {DEFAULT_TRAVEL_S:g})")
    respond_parser.add_argument("--answer-window-ms", type=milliseconds, metavar="MS",
                                help="ignore an answer that starts later than this after the challenge ended (default: no limit)")
    respond_parser.add_argument("--scan", action="store_true",
                                help="listen on all three channels and answer on the request's (a host's retries on "
                                     "other channels then reach the stand-in); default: --freq only")
    respond_parser.add_argument("--seconds", type=float, default=0.0, help="stop after this long (default: Ctrl-C)")
    for p in (execute_parser, respond_parser):
        p.add_argument("--key-file", type=Path, help=f"a file holding the system key (else the variable {KEY_ENV})")
    return parser


def set_tx_power(peer: conformance.Peer, radio: dict, dbm: int, out) -> None:
    """Set the radio's TX power and print what the co-processor applied (it clamps to the radio's range)."""
    params = [{"id": "tx_power_dbm", "value": dbm}]
    ack, errors = peer.request("set_params", {"radio": radio["radio"], "params": params}, "params_ack")
    if ack is None or errors:
        raise BenchError(f"set_params tx_power_dbm refused: {[e.fields for e in errors] or 'no answer'}")
    applied = next((p["value"] for p in ack.fields.get("applied", []) if p.get("id") == "tx_power_dbm"), dbm)
    print(f"TX power {applied} dBm (asked {dbm})", file=out)


def run_command(peer: conformance.Peer, args: argparse.Namespace, key: bytes | None, out) -> int:
    """Boot the co-processor, set it up for `args.command` and run it."""
    ready = benchkit.boot_ready(peer, not args.no_reset)
    print(benchkit.describe_ready(ready), file=out)
    radio = benchkit.radio_of(ready)
    listening = args.command == "respond"
    benchkit.set_up(peer, radio, args.sfd, args.freq, listen=listening,
                    event_mask={"crc_error_rx": True, "frame_start": listening})
    if args.tx_power_dbm is not None:
        set_tx_power(peer, radio, args.tx_power_dbm, out)
    if args.command == "poll":
        return poll(peer, radio, args, out)
    if args.command == "execute":
        return execute(peer, radio, args, key, out)
    where = "on 2425/2450/2475 MHz (scanning)" if args.scan else f"on {args.freq} Hz"
    print(f"answering as {args.node.hex().upper()} {where}; Ctrl-C to stop", file=out)
    Responder(peer, radio, args.freq, args.node, key, out, answer_window_ms=args.answer_window_ms,
              state=args.state, position_pct=args.position, travel_s=args.travel_s, scan=args.scan).run(args.seconds)
    return EXIT_OK


def main(argv: list[str], open_peer=benchkit.open_peer, environ: Mapping[str, str] = os.environ, out=None) -> int:
    out = out or sys.stdout
    args = build_parser().parse_args(argv[1:])
    peer = None
    try:
        if args.refused_key is not None:
            raise BenchError(f"the key is never taken from the command line: use --key-file or {KEY_ENV}")
        key = load_key(args.key_file, environ) if args.command != "poll" else None
        peer = open_peer(args)
        return run_command(peer, args, key, out)
    except BenchError as failure:
        print(f"io2g4: {failure}", file=sys.stderr)
        return EXIT_BENCH
    finally:
        if peer is not None:
            if args.trace:
                benchkit.write_trace(args.trace, peer.recording, out)
            peer.link.close()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
