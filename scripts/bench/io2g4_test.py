#!/usr/bin/env python3
"""Self-test of scripts/bench/io2g4.py against the Bridle package's in-process bench (two fake co-processors and
an air between them, on one fake clock; bridle.testing.fake_world): `poll` and `execute` on one
board, `respond` on the other. No hardware, no network, no real time. Run via `make py-test`."""

from __future__ import annotations

import contextlib
import io
import re
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "corpus"))
sys.path.insert(0, str(ROOT / "scripts" / "bench"))
import protolib  # noqa: E402
import yaml  # noqa: E402
import io2g4  # noqa: E402
from bridle import air, benchkit, bridle, conformance, session  # noqa: E402
from bridle.testing import fake_world  # noqa: E402

CORPUS = ROOT / "tests" / "corpus" / "captures"
KEY = protolib.CORPUS_SYSTEM_KEY  # the public key of the corpus: no test uses another
OTHER_KEY = bytes(range(16))
DEVICE, CONTROLLER = bytes.fromhex("516B10"), bytes.fromhex("4D84A6")
CHALLENGE = bytes.fromhex("6F6D64071EF4")
PUMP_US = 200  # how often the stand-in device looks at what its radio heard


def corpus_frames(name: str) -> list[bytes]:
    capture = yaml.safe_load((CORPUS / name).read_text())
    return [bytes.fromhex(frame["hex"].replace(" ", "")) for frame in capture["frames"]]


class Frames(unittest.TestCase):
    """The frames this tool builds, against the frames the corpus holds byte for byte."""

    def test_the_execute_is_the_corpus_captures_execute(self):
        request, challenge, answer, _ = corpus_frames("exchange/velux_klr200_exchange_execute_open_2g4.yaml")
        self.assertEqual(io2g4.execute_request(DEVICE, CONTROLLER, io2g4.MAIN_OPEN), request)
        self.assertEqual(io2g4.challenge_response(DEVICE, CONTROLLER, request, challenge[9:15], KEY), answer)

    def test_the_status_read_is_the_corpus_captures_request(self):
        request, _ = corpus_frames("probe/velux_klf200_probe_status_poll_timed_2g4.yaml")
        self.assertEqual(io2g4.status_request(bytes.fromhex("0565E7"), bytes.fromhex("82E900")), request)

    def test_every_built_frame_states_its_own_length(self):
        for frame in (io2g4.status_request(DEVICE, CONTROLLER), io2g4.execute_request(DEVICE, CONTROLLER, 0x1234),
                      io2g4.build_frame(0x3D, DEVICE, CONTROLLER, bytes(6)), io2g4.build_frame(0x04, DEVICE, CONTROLLER)):
            self.assertEqual(protolib.ctrl0_implied_length(frame[0]), len(frame), frame.hex())
        with self.assertRaises(ValueError):
            io2g4.build_frame(0x00, DEVICE, CONTROLLER, bytes(24))

    def test_the_main_parameter_follows_the_choice_of_move(self):
        def main(*argv):
            parser = io2g4.build_parser()
            args = parser.parse_args(["io2g4.py", "execute", "--port", "p", "--dst", "516B10", *argv][1:])
            return io2g4.main_parameter(args)
        self.assertEqual((main("--open"), main("--close"), main("--stop")), (0x0000, 0xC800, 0xD200))
        self.assertEqual((main("--position", "0"), main("--position", "50"), main("--position", "100")),
                         (0x0000, 0x6400, 0xC800))
        self.assertEqual(io2g4.execute_data(0xC800), bytes.fromhex("0163C800" "80000000"))

    def test_the_long_train_is_the_specs_and_the_legacy_one_is_the_old_values(self):
        vector = conformance._vector("tx_io_wake_strobe")["strobe"]
        self.assertEqual(io2g4.wake_strobe(legacy=False), vector)
        self.assertEqual((vector["count"], vector["period_ns"], vector["main_gap_ns"]), (510, 1_000_000, 1_036_000))
        legacy = io2g4.wake_strobe(legacy=True)
        self.assertEqual((legacy["count"], legacy["period_ns"], legacy["main_gap_ns"], legacy["counter_unit_ns"]),
                         (512, 976563, 1_344_000, 976563))
        self.assertEqual({k: v for k, v in legacy.items() if k not in io2g4.LEGACY_STROBE_VALUES},
                         {k: v for k, v in vector.items() if k not in io2g4.LEGACY_STROBE_VALUES})


class Received(unittest.TestCase):
    def _rx(self, payload: bytes, crc: str = "ok", t_us: int = 1000) -> dict:
        return {"radio": 0, "t_us": t_us, "freq_hz": bridle.CH20, "rssi_dbm": -60, "lqi": 0, "crc": crc,
                "payload": payload.hex()}

    def _on_air(self, frame: bytes) -> bytes:
        return air.with_fcs(io2g4.psdu_of(frame))

    def test_an_io_frame_is_read_with_its_times(self):
        frame = io2g4.build_frame(0x04, CONTROLLER, DEVICE, bytes(8), end=True)
        got = io2g4.Frame.from_rx(self._rx(self._on_air(frame)))
        self.assertEqual((got.raw, got.dst, got.src, got.cmd, got.data), (frame, CONTROLLER, DEVICE, 0x04, bytes(8)))
        self.assertEqual(got.start_us, 1000 - air.SYNC_US)
        self.assertEqual(got.end_us, 1000 + (1 + len(frame) + 3 + 2) * air.BYTE_US)

    def test_what_is_not_an_io_frame_is_none(self):
        frame = io2g4.build_frame(0x04, CONTROLLER, DEVICE, bytes(8))
        good = self._on_air(frame)
        for name, fields in {"bad FCS": self._rx(good, crc="bad"), "wake frame": self._rx(air.with_fcs(bytes(3))),
                             "other header": self._rx(air.with_fcs(b"\x01\x69\x70" + frame)),
                             "wrong length": self._rx(air.with_fcs(io2g4.psdu_of(frame) + b"\x00")),
                             "too short": self._rx(air.with_fcs(io2g4.psdu_of(frame[:5])))}.items():
            self.assertIsNone(io2g4.Frame.from_rx(fields), name)

    def test_times_on_the_wrapping_clock_are_signed_differences(self):
        self.assertEqual(io2g4.gap_us(100, 4_294_967_000), 396)
        self.assertEqual(io2g4.gap_us(4_294_967_000, 100), -396)
        late = io2g4.Frame(bytes(9), 3, -50, 20)
        self.assertEqual(late.start_us, (3 - air.SYNC_US) % air.U32)

    def test_events_counts_the_damaged_frames_it_passes(self):
        frame = io2g4.build_frame(0x04, CONTROLLER, DEVICE, bytes(8))
        good = self._on_air(frame)
        messages = [conformance.Message("rx", 1, self._rx(good, "bad"), 0.0),
                    conformance.Message("log", 2, {}, 0.0),
                    conformance.Message("rx", 3, self._rx(good), 0.0)]
        events = io2g4.Events(SimpleNamespace(clock=lambda: 0.0, receive=lambda timeout: None, last_command_at=0.0), messages)
        self.assertEqual(events.frame(0.05, lambda f: True).raw, frame)
        self.assertEqual(events.bad_fcs, 1)
        self.assertIsNone(events.frame(0.05, lambda f: True))  # nothing left

    def test_a_long_wait_sends_a_get_stats_before_the_co_processor_lets_go_of_the_host_link(self):
        now, sent = [0.0], []
        peer = SimpleNamespace(clock=lambda: now[0], receive=lambda timeout: None, last_command_at=0.0,
                               send=lambda msg, fields: sent.append(msg))
        events = io2g4.Events(peer)
        events.next(0.0)
        self.assertEqual(sent, [])
        now[0] = 26.0
        events.next(0.0)
        self.assertEqual(sent, ["get_stats"])


class Key(unittest.TestCase):
    HEX = KEY.hex()

    def _load(self, text=None, environ=None):
        with tempfile.TemporaryDirectory() as tmp:
            path = None
            if text is not None:
                path = Path(tmp) / "key.txt"
                path.write_text(text)
            return io2g4.load_key(path, environ or {})

    def test_a_file_or_the_environment_holds_the_key(self):
        self.assertEqual(self._load(self.HEX + "\n"), KEY)
        self.assertEqual(self._load(" ".join(self.HEX[i:i + 4] for i in range(0, 32, 4))), KEY)
        self.assertEqual(self._load(environ={io2g4.KEY_ENV: self.HEX.upper()}), KEY)
        self.assertEqual(self._load(OTHER_KEY.hex(), {io2g4.KEY_ENV: self.HEX}), OTHER_KEY, "the file wins")

    def test_a_missing_or_malformed_key_is_refused_without_naming_it(self):
        for text, environ, wanted in ((None, {}, "no system key"), ("abc", {}, "32 hex digits"),
                                      (self.HEX + "00", {}, "32 hex digits"), ("x" * 32, {}, "32 hex digits"),
                                      (None, {io2g4.KEY_ENV: "12"}, "32 hex digits")):
            with self.assertRaisesRegex(io2g4.BenchError, wanted) as raised:
                self._load(text, environ)
            self.assertNotIn(self.HEX, str(raised.exception))

    def test_a_key_typed_where_a_path_belongs_is_not_echoed(self):
        with self.assertRaises(io2g4.BenchError) as raised:
            io2g4.load_key(Path(self.HEX), {})
        self.assertNotIn(self.HEX, str(raised.exception))


def _run(world, *argv, environ=None, key_file_text=None):
    """io2g4.main() on the world's `dut` board. Returns (exit status, stdout, stderr, the trace's text)."""
    out, err = io.StringIO(), io.StringIO()
    with tempfile.TemporaryDirectory() as tmp:
        trace = Path(tmp) / "run.trace"
        extra = ["--trace", str(trace)]
        if key_file_text is not None:
            key_path = Path(tmp) / "key.txt"
            key_path.write_text(key_file_text)
            extra += ["--key-file", str(key_path)]
        with contextlib.redirect_stderr(err):
            code = io2g4.main(["io2g4.py", *argv, "--port", "unused", *extra],
                              open_peer=lambda args: conformance.Peer(world.dut), environ=environ or {}, out=out)
        return code, out.getvalue(), err.getvalue(), trace.read_text() if trace.exists() else ""


PRE_LISTEN_S = 0.05


def stand_in(world, key=KEY, node=DEVICE, cls=None, random_bytes=lambda n: CHALLENGE, window_ms=None):
    """A device on the world's second board: set up as `respond` does, and looked at every PUMP_US."""
    peer = conformance.Peer(world.ref)
    ready = benchkit.boot_ready(peer)
    radio = benchkit.radio_of(ready)
    benchkit.set_up(peer, radio, 0x56, bridle.CH20, listen=True, event_mask={"crc_error_rx": True})
    out = io.StringIO()
    device = (cls or io2g4.Responder)(peer, radio, bridle.CH20, node, key, out, random_bytes, answer_window_ms=window_ms)

    def look():
        device.step()
        world.schedule(world.now_us + PUMP_US, look)
    world.schedule(world.now_us + PUMP_US, look)
    device.out_text = out.getvalue
    world.advance(PRE_LISTEN_S)  # the board has been listening for a while before the host sends anything
    return device


def io_commands_sent(board) -> list[int]:
    """The io command byte of every frame a board was told to transmit (`tx` without the wake frames)."""
    cmds = []
    for name, fields in board.received:
        if name == "tx":
            frame = bytes.fromhex(fields["payload"])[len(protolib.IO_2G4_PSDU_HEADER):]
            cmds.append(frame[8])
    return cmds


class TxPower(unittest.TestCase):
    def test_a_lower_tx_power_is_set_after_the_vector_and_reported(self):
        world = fake_world.World()
        stand_in(world)
        code, out, err, _ = _run(world, "poll", "--dst", DEVICE.hex(), "--src", "82E900", "--repeat", "1",
                                 "--tx-power-dbm", "-11")
        self.assertEqual((code, err), (io2g4.EXIT_OK, ""), out)
        powers = [p["value"] for name, fields in world.dut.received if name == "set_params"
                  for p in fields["params"] if p["id"] == "tx_power_dbm"]
        self.assertEqual(powers[-1], -11, powers)
        self.assertIn("TX power -12 dBm (asked -11)", out, "the radio's step, as the co-processor applied it")

    def test_without_the_option_only_the_vectors_power_is_sent(self):
        world = fake_world.World()
        stand_in(world)
        _, out, _, _ = _run(world, "poll", "--dst", DEVICE.hex(), "--src", "82E900", "--repeat", "1")
        powers = [p["value"] for name, fields in world.dut.received if name == "set_params"
                  for p in fields["params"] if p["id"] == "tx_power_dbm"]
        self.assertEqual(powers, [8])
        self.assertNotIn("TX power", out)


class FakeLink:
    """Just enough of a conformance.Peer for the Responder: records what is sent, serves queued messages."""

    def __init__(self):
        self.now, self.sent, self.queue, self.last_command_at, self.seq = 0.0, [], [], 0.0, 0

    def clock(self):
        return self.now

    def send(self, name, fields, seq=None):
        self.seq += 1
        self.sent.append((name, fields))
        self.last_command_at = self.now
        return self.seq

    def take(self):
        out, self.queue = self.queue, []
        return out

    def receive(self, wait_s):
        return None


def rx_message(freq_hz, frame):
    """An `rx` event carrying the io `frame` (as build_frame() returns it) on `freq_hz`."""
    payload = io2g4.psdu_of(frame) + b"\x00\x00"  # the FCS; `crc` below says it was good
    fields = {"radio": 0, "t_us": 1000, "freq_hz": freq_hz, "rssi_dbm": -60, "lqi": 0, "crc": "ok",
              "payload": payload.hex()}
    return conformance.Message("rx", 0, fields, 0.0)


class Scan(unittest.TestCase):
    def setUp(self):
        self.link = FakeLink()
        self.device = io2g4.Responder(self.link, {"radio": 0}, bridle.CH20, DEVICE, KEY, io.StringIO(), scan=True)

    def test_a_scanning_stand_in_starts_with_the_hosts_idle_scan(self):
        self.device.start_scan()
        name, fields = self.link.sent[-1]
        self.assertEqual(name, "rx_scan")
        self.assertEqual((fields["dwell_us"], fields["hold_us"], fields["freqs_hz"]),
                         (1500, 8000, [bridle.CH15, bridle.CH20, bridle.CH25]))

    def test_it_answers_on_the_channel_the_request_came_in_on(self):
        request = io2g4.status_request(DEVICE, bytes.fromhex("BE0001"), hub=True)
        self.link.queue.append(rx_message(bridle.CH25, request))
        self.device.step()
        tx = [fields for name, fields in self.link.sent if name == "tx"]
        self.assertEqual(len(tx), 1)
        self.assertEqual(tx[0]["freq_hz"], bridle.CH25)

    def test_after_its_own_transmission_it_scans_again_once_the_exchange_had_time(self):
        request = io2g4.status_request(DEVICE, bytes.fromhex("BE0001"), hub=True)
        self.link.queue.append(rx_message(bridle.CH15, request))
        self.device.step()
        self.link.now += io2g4.RESCAN_AFTER_S / 2
        self.device.step()
        self.assertNotIn("rx_scan", [name for name, _ in self.link.sent], "too early: the exchange may go on")
        self.link.now += io2g4.RESCAN_AFTER_S
        self.device.step()
        self.assertEqual([name for name, _ in self.link.sent].count("rx_scan"), 1)

    def test_without_scan_it_answers_on_its_frequency(self):
        device = io2g4.Responder(self.link, {"radio": 0}, bridle.CH20, DEVICE, KEY, io.StringIO())
        request = io2g4.status_request(DEVICE, bytes.fromhex("BE0001"), hub=True)
        self.link.queue.append(rx_message(bridle.CH25, request))
        device.step()
        tx = [fields for name, fields in self.link.sent if name == "tx"]
        self.assertEqual(tx[0]["freq_hz"], bridle.CH20)


class Poll(unittest.TestCase):
    ARGS = ("poll", "--dst", DEVICE.hex(), "--src", "82E900")

    def test_a_device_that_answers_gives_a_reply_and_exit_status_0(self):
        world = fake_world.World()
        device = stand_in(world)
        code, out, err, trace = _run(world, *self.ARGS, "--repeat", "1")
        self.assertEqual((code, err), (io2g4.EXIT_OK, ""), out)
        self.assertIn("sent 52 20 51 6B 10 82 E9 00 03 01 FE 01 01 01 01 01 01 01 00 behind 510 wake frames", out)
        self.assertRegex(out, r"REPLY 90 00 82 E9 00 51 6B 10 04 2D 60 00 00 80 00 00 00 at -50 dBm, \d+ us after the request ended")
        self.assertIn("1 of 1 status reads answered", out)
        self.assertIn("status read from 82E900: answered 0x04", device.out_text())
        self.assertEqual(session.check(session.from_log(trace)), [], "the trace passes the session linter")

    def test_the_reply_comes_about_when_the_stand_in_looked(self):
        world = fake_world.World()
        stand_in(world)
        _, out, _, _ = _run(world, *self.ARGS, "--repeat", "1")
        gap = int(out.split("REPLY ")[1].split(" us after")[0].split(" ")[-1])
        self.assertTrue(0 < gap <= PUMP_US + fake_world.IMMEDIATE_US + 100, gap)

    def test_the_status_read_goes_out_behind_the_spec_vectors_train_on_channel_20(self):
        world = fake_world.World()
        stand_in(world)
        _run(world, *self.ARGS, "--repeat", "1")
        tx = next(fields for name, fields in world.dut.received if name == "tx")
        self.assertEqual((tx["freq_hz"], tx["then_rx"], tx["strobe"]), (bridle.CH20, True, io2g4.wake_strobe(False)))
        self.assertEqual(world.dut.sfd, 0x56)

    def test_legacy_timing_sends_the_old_train(self):
        world = fake_world.World()
        stand_in(world)
        _, out, _, _ = _run(world, *self.ARGS, "--repeat", "1", "--legacy-timing")
        tx = next(fields for name, fields in world.dut.received if name == "tx")
        self.assertEqual((tx["strobe"]["count"], tx["strobe"]["period_ns"], tx["strobe"]["main_gap_ns"]),
                         (512, 976563, 1344000))
        self.assertIn("behind 512 wake frames", out)

    def test_repeats_are_at_least_two_seconds_apart(self):
        world = fake_world.World()
        stand_in(world)
        before = world.time_s
        _, out, _, _ = _run(world, *self.ARGS, "--repeat", "3")
        self.assertEqual(out.count("REPLY"), 3)
        self.assertGreaterEqual(world.time_s - before, 2 * io2g4.POLL_GAP_S)
        self.assertIn("3 of 3 status reads answered", out)

    def test_no_answer_is_no_reply_and_exit_status_1(self):
        world = fake_world.World()
        code, out, _, _ = _run(world, *self.ARGS, "--repeat", "2")
        self.assertEqual(code, io2g4.EXIT_NO_ANSWER)
        self.assertEqual(out.count("NO REPLY within 50 ms"), 2)
        self.assertIn("0 of 2 status reads answered", out)

    def test_a_challenge_is_reported_and_nothing_more_is_sent(self):
        class Challenges(io2g4.Responder):
            def handle(self, frame):
                self.send(protolib.CMD_CHALLENGE_REQ, frame.src, CHALLENGE)
        world = fake_world.World()
        stand_in(world, cls=Challenges)
        code, out, _, _ = _run(world, *self.ARGS, "--repeat", "3")
        self.assertEqual(code, io2g4.EXIT_CHALLENGE)
        self.assertIn("CHALLENGE 0E 00 82 E9 00 51 6B 10 3C 6F 6D 64 07 1E F4", out)
        self.assertEqual(io_commands_sent(world.dut), [protolib.CMD_PRIVATE], "one status read and no answer to the challenge")
        self.assertNotIn("try 2/3", out)

    def test_another_frame_from_the_device_is_named_not_taken_for_a_reply(self):
        class Errors(io2g4.Responder):
            def handle(self, frame):
                self.send(protolib.CMD_ERROR_RESP, frame.src, bytes([0x07]), end=True)
        world = fake_world.World()
        stand_in(world, cls=Errors)
        code, out, _, _ = _run(world, *self.ARGS, "--repeat", "1")
        self.assertEqual(code, io2g4.EXIT_NO_ANSWER)
        self.assertIn("OTHER FRAME from the device, command 0xfe", out)

    def test_a_frame_for_another_controller_is_not_a_reply(self):
        world = fake_world.World()
        stand_in(world)  # answers whoever asks, to the asker
        code, out, _, _ = _run(world, "poll", "--dst", "0565E7", "--src", "82E900", "--repeat", "1")
        self.assertEqual(code, io2g4.EXIT_NO_ANSWER, "the stand-in is node 516B10, nobody answers 0565E7")
        self.assertIn("NO REPLY", out)

    def test_poll_needs_no_key_and_asks_for_none(self):
        world = fake_world.World()
        stand_in(world)
        code, _, err, _ = _run(world, *self.ARGS, "--repeat", "1", environ={})
        self.assertEqual((code, err), (io2g4.EXIT_OK, ""))


class Execute(unittest.TestCase):
    ARGS = ("execute", "--dst", DEVICE.hex(), "--open")

    def test_a_right_key_moves_the_device_and_the_delay_is_measured(self):
        world = fake_world.World()
        device = stand_in(world)
        code, out, err, _ = _run(world, *self.ARGS, key_file_text=KEY.hex())
        self.assertEqual((code, err), (io2g4.EXIT_OK, ""), out)
        self.assertIn("moving the device 516B10: open", out)
        self.assertIn("sent 50 20 51 6B 10 C0 FF EE 00 01 63 00 00 80 00 00 00 behind 510 wake frames", out)
        self.assertRegex(out, r"challenge 0E 00 C0 FF EE 51 6B 10 3C 6F 6D 64 07 1E F4 at -50 dBm; answered "
                              r"0E 00 51 6B 10 C0 FF EE 3D [0-9A-F ]{17}, \d+ us after the challenge ended \(on air\)")
        self.assertIn("DONE 90 00 C0 FF EE 51 6B 10 04 54 60 00 00 80 00 00 00", out)
        self.assertIn("answer from C0FFEE is right: moved", device.out_text())
        self.assertEqual(io_commands_sent(world.dut), [protolib.CMD_EXECUTE, protolib.CMD_CHALLENGE_RESP])
        tx = [fields for name, fields in world.dut.received if name == "tx"]
        self.assertIn("strobe", tx[0])
        self.assertNotIn("strobe", tx[1], "the answer continues the exchange: no wake train")

    def test_the_answer_follows_the_challenge_by_about_the_turnaround_of_the_bench(self):
        world = fake_world.World()
        stand_in(world)
        _, out, _, _ = _run(world, *self.ARGS, key_file_text=KEY.hex())
        delay = int(out.split("answered ")[1].split(" us after")[0].split(", ")[-1])
        self.assertTrue(0 < delay <= 2 * fake_world.IMMEDIATE_US + 100, delay)

    def test_the_key_can_come_from_the_environment_and_the_source_can_be_chosen(self):
        world = fake_world.World()
        stand_in(world)
        code, out, _, _ = _run(world, *self.ARGS, "--src", "4D84A6", environ={io2g4.KEY_ENV: KEY.hex()})
        self.assertEqual(code, io2g4.EXIT_OK, out)
        self.assertIn("sent 50 20 51 6B 10 4D 84 A6 00 01 63 00 00 80 00 00 00", out)

    def test_close_stop_and_position_send_their_main_parameter(self):
        for argv, main in ((("--close",), "C8 00"), (("--stop",), "D2 00"), (("--position", "25"), "32 00")):
            world = fake_world.World()
            stand_in(world)
            _, out, _, _ = _run(world, "execute", "--dst", DEVICE.hex(), *argv, key_file_text=KEY.hex())
            self.assertIn(f"C0 FF EE 00 01 63 {main} 80 00 00 00", out)

    def test_a_wrong_key_is_no_confirmation_with_the_delay(self):
        world = fake_world.World()
        device = stand_in(world, key=OTHER_KEY)
        code, out, _, _ = _run(world, *self.ARGS, key_file_text=KEY.hex())
        self.assertEqual(code, io2g4.EXIT_NO_ANSWER)
        self.assertRegex(out, r"NO CONFIRMATION within 50 ms of the answer, sent \d+ us after the challenge: "
                              r"the key is wrong, or the answer was too late")
        self.assertNotIn("DONE", out)
        self.assertIn("is wrong: no reply", device.out_text())

    def test_a_device_that_does_not_challenge_is_no_challenge(self):
        world = fake_world.World()
        code, out, _, _ = _run(world, *self.ARGS, "--tries", "1", key_file_text=KEY.hex())
        self.assertEqual(code, io2g4.EXIT_NO_ANSWER)
        self.assertIn("NO CHALLENGE within 50 ms: the device did not accept the request (wake train, address or source)", out)
        self.assertEqual(io_commands_sent(world.dut), [protolib.CMD_EXECUTE], "no answer without a challenge")

    def test_an_error_instead_of_the_challenge_or_the_confirmation_is_error(self):
        class RefusesRequest(io2g4.Responder):
            def handle(self, frame):
                self.send(protolib.CMD_ERROR_RESP, frame.src, bytes([0x38]), end=True)

        class RefusesAnswer(io2g4.Responder):
            def handle(self, frame):
                if frame.cmd == protolib.CMD_CHALLENGE_RESP:
                    self.send(protolib.CMD_ERROR_RESP, frame.src, bytes([0x38]), end=True)
                else:
                    super().handle(frame)
        for cls, wanted in ((RefusesRequest, "ERROR 89 00 C0 FF EE 51 6B 10 FE 38: the device refused the request"),
                            (RefusesAnswer, "ERROR 89 00 C0 FF EE 51 6B 10 FE 38: the device refused the answer")):
            world = fake_world.World()
            stand_in(world, cls=cls)
            code, out, _, _ = _run(world, *self.ARGS, key_file_text=KEY.hex())
            self.assertEqual(code, io2g4.EXIT_NO_ANSWER)
            self.assertIn(wanted, out)

    def test_the_key_never_reaches_the_output_the_trace_or_the_recording(self):
        world = fake_world.World()
        stand_in(world)
        for source in ({"key_file_text": KEY.hex()}, {"environ": {io2g4.KEY_ENV: KEY.hex()}}):
            code, out, err, trace = _run(world, *self.ARGS, **source)
            self.assertEqual(code, io2g4.EXIT_OK)
            for text in (out, err, trace):
                self.assertNotIn(KEY.hex(), text.lower())
                self.assertNotIn(KEY.hex(" "), text.lower())
            self.assertTrue(trace, "a trace was written")
            self.assertIn(CHALLENGE.hex(), trace, "the trace holds the challenge")

    def test_a_key_on_the_command_line_is_refused_whatever_the_spelling(self):
        for spelling in (["--key", KEY.hex()], ["--key=" + KEY.hex()]):
            world = fake_world.World()
            code, out, err, _ = _run(world, *self.ARGS, *spelling)
            self.assertEqual(code, io2g4.EXIT_BENCH)
            self.assertIn("never taken from the command line", err)
            for text in (out, err):
                self.assertNotIn(KEY.hex(), text.lower())
            self.assertEqual(world.dut.received, [], "nothing was sent: the co-processor was not even reset")
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()) as err:
            io2g4.build_parser().parse_args(["execute", "--port", "p", "--dst", "516B10", "--open", "--key-f", "x"])
        self.assertNotIn("x" * 32, err.getvalue())

    def test_without_a_key_nothing_is_sent(self):
        world = fake_world.World()
        code, _, err, _ = _run(world, *self.ARGS)
        self.assertEqual(code, io2g4.EXIT_BENCH)
        self.assertIn("no system key", err)
        self.assertEqual(world.dut.received, [])

    def test_a_position_is_a_percentage(self):
        for bad in ("-1", "100.5", "far"):
            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                io2g4.build_parser().parse_args(["execute", "--port", "p", "--dst", "516B10", "--position", bad])
        world = fake_world.World()
        stand_in(world)
        _, out, _, _ = _run(world, "execute", "--dst", DEVICE.hex(), "--position", "37.5", key_file_text=KEY.hex())
        self.assertIn("moving the device 516B10: to position 37.5", out)
        self.assertIn("00 01 63 4B 00 80 00 00 00", out)  # 37.5 x 512 = 0x4B00

    def test_the_stand_in_challenges_with_fresh_random_bytes(self):
        world = fake_world.World()
        device = stand_in(world, random_bytes=io2g4.os.urandom)
        challenges = set()
        for _ in range(3):
            _, out, _, _ = _run(world, *self.ARGS, key_file_text=KEY.hex())
            self.assertIn("DONE", out)
            challenges.add(out.split("challenge ")[1].split(" at ")[0])
        self.assertEqual(len(challenges), 3)

    def test_exactly_one_move_is_required(self):
        for argv in (("--open", "--close"), ()):
            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                io2g4.build_parser().parse_args(["execute", "--port", "p", "--dst", "516B10", *argv])


class Commands(unittest.TestCase):
    def test_a_run_sends_no_io_command_but_the_three_it_is_for(self):
        world = fake_world.World()
        stand_in(world)
        _run(world, "poll", "--dst", DEVICE.hex(), "--src", "82E900", "--repeat", "1")
        _run(world, "execute", "--dst", DEVICE.hex(), "--open", key_file_text=KEY.hex())
        self.assertEqual(set(io_commands_sent(world.dut)), {protolib.CMD_PRIVATE, protolib.CMD_EXECUTE,
                                                            protolib.CMD_CHALLENGE_RESP})

    def test_the_parser_has_no_option_for_another_command_or_for_pairing(self):
        parser = io2g4.build_parser()
        sub = {name: {a.dest for a in p._actions} for name, p in parser._subparsers._group_actions[0].choices.items()}
        for name, dests in sub.items():
            self.assertFalse({"cmd", "command_byte", "payload", "hex", "pair", "pairing", "key_transfer"} & dests, name)
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            parser.parse_args(["pair", "--port", "p"])

    def test_node_ids_and_repeat_are_checked(self):
        parser = io2g4.build_parser()
        for argv in (["poll", "--port", "p", "--dst", "12345", "--src", "82E900"],
                     ["poll", "--port", "p", "--dst", "GGGGGG", "--src", "82E900"],
                     ["poll", "--port", "p", "--dst", "516B10", "--src", "82E900", "--repeat", "0"]):
            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                parser.parse_args(argv)

    def test_a_refused_transmission_is_a_bench_error(self):
        class Refuses(fake_world.FakeCoproc):
            def _tx(self, seq, f):
                self.emit("error", {"cmd_seq": seq, "code": "channel_busy", "detail": 0})
        world = fake_world.World()
        world.dut = Refuses(world, "device", epoch_us=0)
        code, _, err, _ = _run(world, "poll", "--dst", DEVICE.hex(), "--src", "82E900")
        self.assertEqual(code, io2g4.EXIT_BENCH)
        self.assertIn("refused the transmission: channel_busy", err)

    def test_a_co_processor_that_never_finishes_a_transmission_is_a_bench_error(self):
        class Silent(fake_world.FakeCoproc):
            def _tx(self, seq, f):
                pass
        world = fake_world.World()
        world.dut = Silent(world, "device", epoch_us=0)
        code, _, err, _ = _run(world, "poll", "--dst", DEVICE.hex(), "--src", "82E900")
        self.assertEqual(code, io2g4.EXIT_BENCH)
        self.assertIn("no tx_done within 5 s", err)

    def test_a_boot_that_fails_is_a_bench_error_and_the_trace_is_still_written(self):
        class Mute(fake_world.FakeCoproc):
            def handle(self, msg, seq, f):
                pass
        world = fake_world.World()
        world.dut = Mute(world, "device", epoch_us=0)
        with tempfile.TemporaryDirectory() as tmp:
            trace = Path(tmp) / "t.trace"
            with contextlib.redirect_stderr(io.StringIO()) as err:
                code = io2g4.main(["io2g4.py", "poll", "--port", "p", "--dst", "516B10", "--src", "82E900", "--trace", str(trace)],
                                  open_peer=lambda args: conformance.Peer(world.dut), environ={}, out=io.StringIO())
            self.assertEqual(code, io2g4.EXIT_BENCH)
            self.assertIn("no ready within", err.getvalue())
            self.assertIn("[bridle] > ", trace.read_text(), "the soft_reset is in the trace")


def answer_tx(world) -> list[dict]:
    """The `tx` commands that carry a 0x3D, in order."""
    header = len(protolib.IO_2G4_PSDU_HEADER)
    return [f for name, f in world.dut.received
            if name == "tx" and bytes.fromhex(f["payload"])[header + 8] == protolib.CMD_CHALLENGE_RESP]


def gaps_us(text: str) -> list[int]:
    """The `gap 0x3C end -> 0x3D start` figures a responder printed."""
    return [int(line.rsplit(": ", 1)[1].split(" ")[0]) for line in text.splitlines() if line.startswith("gap 0x3C")]


class Tries(unittest.TestCase):
    ARGS = ("execute", "--dst", DEVICE.hex(), "--open")

    class AnswersThird(io2g4.Responder):
        asked = 0

        def handle(self, frame):
            if frame.cmd == protolib.CMD_EXECUTE:
                self.asked += 1
                if self.asked < 3:
                    return
            super().handle(frame)

    def test_the_retry_delay_is_the_hubs(self):
        header = (ROOT / "components" / "home_io_control" / "proto_timing.h").read_text()
        hub_ms = int(re.search(r"EXCHANGE_RETRY_DELAY_MS\s*=\s*(\d+)", header).group(1))
        self.assertEqual(round(io2g4.RETRY_DELAY_S * 1000), hub_ms)
        self.assertEqual(io2g4.DEFAULT_TRIES, 3)

    def test_a_device_that_answers_only_the_third_request_is_done_on_try_3(self):
        world = fake_world.World()
        stand_in(world, cls=self.AnswersThird)
        start = world.time_s
        code, out, err, _ = _run(world, *self.ARGS, key_file_text=KEY.hex())
        self.assertEqual((code, err), (io2g4.EXIT_OK, ""), out)
        self.assertEqual(out.count("moving the device"), 1)
        self.assertIn("try 1/3: sent 50 20", out)
        self.assertIn("try 1/3: NO CHALLENGE", out)
        self.assertIn("try 2/3: NO CHALLENGE", out)
        self.assertIn("try 3/3: challenge 0E 00", out)
        self.assertIn("try 3/3: DONE 90 00", out)
        self.assertEqual(io_commands_sent(world.dut), [0, 0, 0, protolib.CMD_CHALLENGE_RESP])
        # two silent tries: each a 50 ms window and the hub's retry delay, besides the trains
        self.assertGreaterEqual(world.time_s - start, 2 * (io2g4.RETRY_DELAY_S + io2g4.REPLY_WINDOW_S))

    def test_a_device_that_never_answers_is_asked_tries_times_and_the_status_is_the_last_tries(self):
        for tries in (1, 2, 3):
            world = fake_world.World()
            code, out, _, _ = _run(world, *self.ARGS, "--tries", str(tries), key_file_text=KEY.hex())
            self.assertEqual(code, io2g4.EXIT_NO_ANSWER)
            self.assertEqual(out.count("NO CHALLENGE"), tries)
            self.assertIn(f"try {tries}/{tries}: NO CHALLENGE", out)
            self.assertEqual(len(io_commands_sent(world.dut)), tries)
        world = fake_world.World()
        _, out, _, _ = _run(world, *self.ARGS, key_file_text=KEY.hex())
        self.assertEqual(out.count("NO CHALLENGE"), 3, "three tries by default")

    def test_a_wrong_answer_is_asked_again_from_the_request_with_a_new_answer(self):
        world = fake_world.World()
        stand_in(world, key=OTHER_KEY)
        code, out, _, _ = _run(world, *self.ARGS, key_file_text=KEY.hex())
        self.assertEqual(code, io2g4.EXIT_NO_ANSWER)
        self.assertEqual(out.count("NO CONFIRMATION"), 3)
        self.assertEqual(io_commands_sent(world.dut), [0, protolib.CMD_CHALLENGE_RESP] * 3)

    def test_an_error_from_the_device_ends_the_run_without_a_retry(self):
        class RefusesRequest(io2g4.Responder):
            def handle(self, frame):
                self.send(protolib.CMD_ERROR_RESP, frame.src, bytes([0x38]), end=True)

        class RefusesAnswer(io2g4.Responder):
            def handle(self, frame):
                if frame.cmd == protolib.CMD_CHALLENGE_RESP:
                    self.send(protolib.CMD_ERROR_RESP, frame.src, bytes([0x38]), end=True)
                else:
                    super().handle(frame)
        for cls, sent in ((RefusesRequest, [0]), (RefusesAnswer, [0, protolib.CMD_CHALLENGE_RESP])):
            world = fake_world.World()
            stand_in(world, cls=cls)
            code, out, _, _ = _run(world, *self.ARGS, key_file_text=KEY.hex())
            self.assertEqual(code, io2g4.EXIT_NO_ANSWER)
            self.assertEqual(out.count("ERROR"), 1)
            self.assertNotIn("try 2/3", out)
            self.assertEqual(io_commands_sent(world.dut), sent)

    def test_tries_is_at_least_1(self):
        for bad in ("0", "-1", "x"):
            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                io2g4.build_parser().parse_args(["execute", "--port", "p", "--dst", "516B10", "--open", "--tries", bad])


class AnswerDelay(unittest.TestCase):
    ARGS = ("execute", "--dst", DEVICE.hex(), "--open")

    def _run_with(self, delay, window_ms=None, tries="1"):
        world = fake_world.World()
        device = stand_in(world, window_ms=window_ms)
        result = _run(world, *self.ARGS, "--tries", tries, "--answer-delay-ms", delay, key_file_text=KEY.hex())
        return world, device, result

    def test_the_answer_starts_the_delay_after_the_challenge_ended_as_the_responder_sees_it(self):
        for delay_ms in (5, 20, 87.5):
            world, device, (code, out, _, _) = self._run_with(str(delay_ms))
            self.assertEqual(code, io2g4.EXIT_OK, out)
            tx = answer_tx(world)[0]
            self.assertIn("t_main_us", tx)
            self.assertNotIn("strobe", tx)
            (gap,) = gaps_us(device.out_text())
            self.assertAlmostEqual(gap, delay_ms * 1000, delta=50)
            self.assertRegex(out, rf"\d+ us after the challenge ended \(on air\); asked for {delay_ms:g} ms")
            measured = int(out.split("answered ")[1].split(" us after")[0].split(", ")[-1])
            self.assertEqual(measured, gap, "the tool and the responder measure the same gap")
            self.assertNotIn("sent at once", out)

    def test_a_delay_the_link_cannot_time_is_sent_at_once_and_says_so(self):
        for delay_ms in ("0", "2"):
            world, device, (code, out, _, _) = self._run_with(delay_ms)
            self.assertEqual(code, io2g4.EXIT_OK, out)
            self.assertNotIn("t_main_us", answer_tx(world)[0])
            self.assertRegex(out, rf"try 1/1: --answer-delay-ms {delay_ms} is BELOW what this link can time "
                                  rf"\(it cannot start an answer sooner than about [\d.]+ ms after the challenge ended\): "
                                  rf"the answer WAS SENT AT ONCE, not after {delay_ms} ms")
            self.assertRegex(out, rf"\d+ us after the challenge ended \(on air\); asked for {delay_ms} ms, sent at once "
                                  rf"\(below the floor of this link\)")
            self.assertNotIn("asked for 7", out)
            self.assertLess(gaps_us(device.out_text())[0], 1000)

    def test_without_the_option_the_answer_is_sent_at_once(self):
        world = fake_world.World()
        stand_in(world)
        _run(world, *self.ARGS, key_file_text=KEY.hex())
        self.assertNotIn("t_main_us", answer_tx(world)[0])

    def test_the_time_asked_for_follows_the_challenge_on_the_co_processors_clock_across_the_wrap(self):
        challenge = io2g4.Frame(bytes(9), 100, -50, 20, heard_s=1.0)
        t_main, note = io2g4.answer_time(20.0, challenge, 1.0)
        self.assertEqual((t_main, note), ((challenge.end_us + 20_000) % air.U32, ""))
        late = io2g4.Frame(bytes(9), air.U32 - 10_000, -50, 20, heard_s=1.0)
        t_main = io2g4.answer_time(20.0, late, 1.0)[0]
        self.assertEqual(t_main, (late.end_us + 20_000) % air.U32)
        self.assertLess(t_main, late.end_us, "the wrap is crossed")

    def test_the_time_the_pc_has_already_used_counts_against_the_delay(self):
        challenge = io2g4.Frame(bytes(9), 100, -50, 20, heard_s=1.0)
        self.assertIsNotNone(io2g4.answer_time(10.0, challenge, 1.0)[0])
        none, note = io2g4.answer_time(10.0, challenge, 1.0075)  # 7.5 ms later: 7.5 + 3 > 10
        self.assertIsNone(none)
        self.assertIn("SENT AT ONCE", note)

    def test_the_window_of_the_stand_in_is_judged_on_both_sides_of_the_limit(self):
        world, device, (code, out, _, _) = self._run_with("8", window_ms=10)
        self.assertEqual(code, io2g4.EXIT_OK, out)
        self.assertNotIn("ignored", device.out_text())
        world, device, (code, out, _, _) = self._run_with("12", window_ms=10)
        self.assertEqual(code, io2g4.EXIT_NO_ANSWER)
        self.assertIn("NO CONFIRMATION", out)
        self.assertRegex(device.out_text(), r"answer from C0FFEE came 12\d\d\d us after the challenge, past the window of "
                                            r"10 ms: ignored")
        self.assertNotIn("is right", device.out_text())

    def test_a_late_answer_is_asked_for_again_with_a_fresh_request(self):
        world, device, (code, out, _, _) = self._run_with("30", window_ms=10, tries="3")
        self.assertEqual(out.count("NO CONFIRMATION"), 3)
        self.assertEqual(len(answer_tx(world)), 3)
        self.assertEqual(len(gaps_us(device.out_text())), 3)

    def test_no_window_is_no_limit(self):
        _, device, (code, out, _, _) = self._run_with("100")
        self.assertEqual(code, io2g4.EXIT_OK, out)
        self.assertAlmostEqual(gaps_us(device.out_text())[0], 100_000, delta=50)

    def test_the_options_are_checked(self):
        for argv in (["execute", "--dst", "516B10", "--open", "--answer-delay-ms", "-1"],
                     ["execute", "--dst", "516B10", "--open", "--answer-delay-ms", "1001"],
                     ["execute", "--dst", "516B10", "--open", "--answer-delay-ms", "soon"],
                     ["respond", "--node", "516B10", "--answer-window-ms", "-5"],
                     ["poll", "--dst", "516B10", "--src", "82E900", "--answer-delay-ms", "5"],
                     ["execute", "--dst", "516B10", "--open", "--answer-window-ms", "5"]):
            with self.assertRaises(SystemExit, msg=argv), contextlib.redirect_stderr(io.StringIO()):
                io2g4.build_parser().parse_args([argv[0], "--port", "p", *argv[1:]])


class LowPowerAndWake(unittest.TestCase):
    ARGS = ("execute", "--dst", DEVICE.hex(), "--open")

    def _ctrl1_of_answer(self, world) -> int:
        return bytes.fromhex(answer_tx(world)[0]["payload"])[len(protolib.IO_2G4_PSDU_HEADER) + 1]

    def test_the_answer_is_sent_clear_unless_asked(self):
        world = fake_world.World()
        device = stand_in(world)
        _run(world, *self.ARGS, key_file_text=KEY.hex())
        self.assertEqual(self._ctrl1_of_answer(world), 0)
        self.assertIn("heard 0x3d from C0FFEE: CTRL1 0x00", device.out_text())

    def test_lp_on_response_sets_the_flag_on_the_answer_and_only_there(self):
        world = fake_world.World()
        device = stand_in(world)
        code, out, _, _ = _run(world, *self.ARGS, "--lp-on-response", key_file_text=KEY.hex())
        self.assertEqual(code, io2g4.EXIT_OK, out)
        self.assertEqual(self._ctrl1_of_answer(world), protolib.CTRL1_LOW_POWER)
        self.assertIn("answered 0E 20 51 6B 10 C0 FF EE 3D", out)
        request = bytes.fromhex(next(f for n, f in world.dut.received if n == "tx")["payload"])
        self.assertEqual(request[len(protolib.IO_2G4_PSDU_HEADER) + 1], protolib.CTRL1_LOW_POWER, "the request is as before")
        self.assertIn("heard 0x3d from C0FFEE: CTRL1 0x20", device.out_text())
        self.assertIn("is right: moved", device.out_text(), "the stand-in takes either")
        self.assertEqual(io2g4.challenge_response(DEVICE, CONTROLLER, bytes(9), CHALLENGE, KEY)[1], 0, "clear by default")

    def test_the_long_train_is_the_default_and_the_short_one_is_8_frames(self):
        for argv, count, name in (((), 510, "long"), (("--wake", "long"), 510, "long"), (("--wake", "short"), 8, "short")):
            world = fake_world.World()
            device = stand_in(world)
            code, out, _, _ = _run(world, *self.ARGS, *argv, key_file_text=KEY.hex())
            self.assertEqual(code, io2g4.EXIT_OK, out)
            tx = next(f for n, f in world.dut.received if n == "tx")
            self.assertEqual(tx["strobe"], io2g4.wake_strobe(False, short=name == "short"))
            self.assertEqual(tx["strobe"]["count"], count)
            self.assertIn(f"behind {count} wake frames ({name} train)", out)
            # the in-process co-processor puts the early frames of a long train in the past, so only the short
            # train's count is exact here; on a bench the responder counts all 510
            wakes = re.search(r"heard 0x00 from C0FFEE: CTRL1 0x20, after (\d+) wake frames", device.out_text())
            self.assertIsNotNone(wakes, device.out_text())
            if name == "short":
                self.assertEqual(int(wakes.group(1)), count)
            else:
                self.assertGreater(int(wakes.group(1)), 8)
            self.assertIn("heard 0x3d from C0FFEE: CTRL1 0x00, after 0 wake frames", device.out_text())

    def test_the_short_train_is_the_specs_vector(self):
        vector = conformance._vector("tx_io_short_wake_strobe")["strobe"]
        self.assertEqual(io2g4.wake_strobe(False, short=True), vector)
        self.assertEqual((vector["count"], vector["period_ns"], vector["main_gap_ns"]), (8, 1_000_000, 1_036_000))

    def test_the_wake_option_is_long_or_short(self):
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            io2g4.build_parser().parse_args(["execute", "--port", "p", "--dst", "516B10", "--open", "--wake", "medium"])


class HubRequest(unittest.TestCase):
    def _corpus(self, name: str) -> list[bytes]:
        return corpus_frames(name)

    def test_the_hubs_status_request_is_the_corpus_captures_request_byte_for_byte(self):
        request, _ = self._corpus("statuspoll/velux_ssl_statuspoll_stop_settle_preamble_escalation.yaml")[:2]
        self.assertEqual(request.hex(" ").upper(), "4B 20 39 43 DA E3 F1 D3 03 03 00 00")
        self.assertEqual(io2g4.status_request(bytes.fromhex("3943DA"), bytes.fromhex("E3F1D3"), hub=True), request)
        self.assertEqual(io2g4.status_request(DEVICE, CONTROLLER, hub=True)[8:], bytes.fromhex("03 03 00 00"))

    def test_the_klf_request_is_still_the_default(self):
        klf, _ = corpus_frames("probe/velux_klf200_probe_status_poll_timed_2g4.yaml")
        self.assertEqual(io2g4.status_request(bytes.fromhex("0565E7"), bytes.fromhex("82E900")), klf)
        self.assertNotEqual(io2g4.status_request(DEVICE, CONTROLLER), io2g4.status_request(DEVICE, CONTROLLER, hub=True))

    def test_the_hub_layout_reply_is_the_corpus_replies_byte_for_byte(self):
        # The data of the 0x04 a 2.4 GHz blind sent to the hub's status read: at rest at 47 %, and the first
        # reply of a blind setting off for the closed limit (3 %, 70 s to go).
        commander = bytes.fromhex("4D84A6")
        rest = self._corpus("statuspoll/velux_skylight_blind_statuspoll_hub_request_resting_2g4.yaml")[-1]
        self.assertEqual(io2g4.hub_status_data(stopped=True, target_raw=0x5E00, current_raw=0x5E1F, commander=commander),
                         rest[9:])
        moving = self._corpus("statuspoll/velux_skylight_blind_statuspoll_hub_request_setting_off_2g4.yaml")[-1]
        self.assertEqual(io2g4.hub_status_data(stopped=False, target_raw=0xC800, current_raw=0x060E, remaining_s=70,
                                               commander=commander), moving[9:])

    def test_the_stand_in_reports_resting_or_moving_at_a_position(self):
        rest = io2g4.responder_status_data("resting", 50, CONTROLLER)
        self.assertEqual((rest[0] & io2g4.HUB_STATUS_STOPPED, rest[2:4], rest[4:6]),
                         (1, bytes.fromhex("6400"), bytes.fromhex("6400")), "stopped, target = current = 50 %")
        self.assertEqual((rest[1], rest[6:8], rest[8:11]), (0x60, bytes.fromhex("0001"), CONTROLLER))
        moving = io2g4.responder_status_data("moving", 25, CONTROLLER)
        self.assertEqual((moving[0] & io2g4.HUB_STATUS_STOPPED, moving[1], moving[2:4], moving[4:6]),
                         (0, 0x60, bytes.fromhex("C800"), bytes.fromhex("3200")))
        self.assertEqual(io2g4.responder_status_data("moving", 75, CONTROLLER)[2:4], bytes.fromhex("0000"))

    def test_a_moving_stand_in_falls_in_remaining_seconds_and_advances_to_its_target(self):
        remaining, position = [], []
        for step in range(40):  # 2.5 s apart, as the poll of a blind setting off
            data = io2g4.responder_status_data("moving", 0, CONTROLLER, elapsed_s=2.5 * step)
            if data[0] & io2g4.HUB_STATUS_STOPPED:
                break
            remaining.append(int.from_bytes(data[6:8], "big"))
            position.append(int.from_bytes(data[4:6], "big"))
        self.assertEqual(remaining[:4], [72, 70, 67, 65])  # from fully open: 72 s, then the blind's own sequence
        self.assertEqual(remaining[-1], 2)
        self.assertEqual(position, sorted(position))
        self.assertTrue(all(a < b for a, b in zip(position, position[1:])))
        arrived = io2g4.responder_status_data("moving", 0, CONTROLLER, elapsed_s=2.5 * (len(remaining) + 1))
        self.assertEqual(arrived, io2g4.hub_status_data(stopped=True, target_raw=0xC800, current_raw=0xC800,
                                                        commander=CONTROLLER))
        self.assertEqual(int.from_bytes(arrived[6:8], "big"), 1)

    def test_a_moving_stand_in_towards_open_counts_down_too(self):
        first = io2g4.responder_status_data("moving", 100, CONTROLLER, elapsed_s=2.5)
        later = io2g4.responder_status_data("moving", 100, CONTROLLER, elapsed_s=5.0)
        self.assertEqual(first[2:4], bytes(2))
        self.assertGreater(int.from_bytes(first[4:6], "big"), int.from_bytes(later[4:6], "big"))
        self.assertGreater(int.from_bytes(first[6:8], "big"), int.from_bytes(later[6:8], "big"))

    def test_the_responder_starts_its_travel_at_the_first_status_read_and_answers_a_moment_alike(self):
        now = [10.0]
        peer = SimpleNamespace(clock=lambda: now[0])
        device = io2g4.Responder(peer, {"radio": 0}, bridle.CH20, DEVICE, KEY, io.StringIO(), state="moving",
                                 position_pct=0, travel_s=72.0)
        first = device.status_data(CONTROLLER)
        self.assertEqual(device.status_data(CONTROLLER), first, "two reads in the same instant")
        self.assertEqual(first, io2g4.responder_status_data("moving", 0, CONTROLLER, 0.0, 72.0))
        now[0] += 36.0
        half = device.status_data(CONTROLLER)
        self.assertEqual(half, io2g4.responder_status_data("moving", 0, CONTROLLER, 36.0, 72.0))
        self.assertEqual(int.from_bytes(half[6:8], "big"), 36)
        now[0] += 40.0
        self.assertTrue(device.status_data(CONTROLLER)[0] & io2g4.HUB_STATUS_STOPPED, "arrived: resting")

    def test_a_resting_stand_in_never_moves(self):
        now = [0.0]
        device = io2g4.Responder(SimpleNamespace(clock=lambda: now[0]), {"radio": 0}, bridle.CH20, DEVICE, KEY,
                                 io.StringIO())
        first = device.status_data(CONTROLLER)
        now[0] = 500.0
        self.assertEqual(device.status_data(CONTROLLER), first)

    def test_poll_with_hub_request_sends_it_behind_the_long_train_and_reads_the_hub_layout_reply(self):
        world = fake_world.World()
        device = stand_in(world)
        code, out, err, trace = _run(world, "poll", "--dst", DEVICE.hex(), "--src", "C0FFEE", "--repeat", "1",
                                     "--hub-request")
        self.assertEqual((code, err), (io2g4.EXIT_OK, ""), out)
        self.assertIn("sent 4B 20 51 6B 10 C0 FF EE 03 03 00 00 behind 510 wake frames", out)
        self.assertRegex(out, r"REPLY 96 00 C0 FF EE 51 6B 10 04 05 60 64 00 64 00 00 01 C0 FF EE 01 00 00 at -50 dBm")
        self.assertIn("in the hub's layout, resting at 50", device.out_text())
        tx = next(f for n, f in world.dut.received if n == "tx")
        self.assertEqual(tx["strobe"], io2g4.wake_strobe(False))
        self.assertEqual(io_commands_sent(world.dut), [protolib.CMD_PRIVATE])
        self.assertEqual(session.check(session.from_log(trace)), [])

    def test_the_klf_shaped_request_still_gets_the_bytes_it_always_got(self):
        world = fake_world.World()
        stand_in(world)
        _, out, _, _ = _run(world, "poll", "--dst", DEVICE.hex(), "--src", "82E900", "--repeat", "1")
        self.assertIn("REPLY 90 00 82 E9 00 51 6B 10 04 2D 60 00 00 80 00 00 00", out)

    def test_the_stand_in_answers_in_the_state_and_position_it_was_started_with(self):
        for state, position, wanted in (("resting", 30, "04 05 60 3C 00 3C 00 00 01"), ("moving", 30, "04 04 60 C8 00 3C 00 00 33")):
            world = fake_world.World()
            peer = conformance.Peer(world.ref)
            ready = benchkit.boot_ready(peer)
            radio = benchkit.radio_of(ready)
            benchkit.set_up(peer, radio, 0x56, bridle.CH20, listen=True, event_mask={})
            device = io2g4.Responder(peer, radio, bridle.CH20, DEVICE, KEY, io.StringIO(), state=state, position_pct=position)

            def look():
                device.step()
                world.schedule(world.now_us + PUMP_US, look)
            world.schedule(world.now_us + PUMP_US, look)
            _, out, _, _ = _run(world, "poll", "--dst", DEVICE.hex(), "--src", "C0FFEE", "--repeat", "1", "--hub-request")
            self.assertIn(wanted, out, (state, out))

    def test_respond_takes_a_state_and_a_position(self):
        parser = io2g4.build_parser()
        args = parser.parse_args(["respond", "--port", "p", "--node", "516B10"])
        self.assertEqual((args.state, args.position, args.travel_s), ("resting", 50.0, 72.0))
        args = parser.parse_args(["respond", "--port", "p", "--node", "516B10", "--state", "moving", "--position", "12.5",
                                  "--travel-s", "30"])
        self.assertEqual((args.state, args.position, args.travel_s), ("moving", 12.5, 30.0))
        for bad in (["--state", "asleep"], ["--position", "101"], ["--travel-s", "0"]):
            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                parser.parse_args(["respond", "--port", "p", "--node", "516B10", *bad])

    def test_still_nothing_but_the_three_commands_goes_out(self):
        world = fake_world.World()
        stand_in(world)
        _run(world, "poll", "--dst", DEVICE.hex(), "--src", "C0FFEE", "--repeat", "1", "--hub-request")
        self.assertEqual(set(io_commands_sent(world.dut)), {protolib.CMD_PRIVATE})


class TraceNames(unittest.TestCase):
    def _run_to(self, trace: Path) -> str:
        world = fake_world.World()
        stand_in(world)
        out = io.StringIO()
        code = io2g4.main(["io2g4.py", "poll", "--port", "p", "--dst", DEVICE.hex(), "--src", "82E900", "--repeat", "1",
                           "--trace", str(trace)], open_peer=lambda args: conformance.Peer(world.dut), environ={}, out=out)
        self.assertEqual(code, io2g4.EXIT_OK)
        return out.getvalue()

    def test_a_trace_that_exists_is_not_overwritten_and_the_name_used_is_printed(self):
        with tempfile.TemporaryDirectory() as tmp:
            trace = Path(tmp) / "t6.trace"
            trace.write_text("keep me\n")
            first = self._run_to(trace)
            second = self._run_to(trace)
            self.assertEqual(trace.read_text(), "keep me\n")
            self.assertIn(f"trace written to {Path(tmp) / 't6-2.trace'}", first)
            self.assertIn(f"trace written to {Path(tmp) / 't6-3.trace'}", second)
            for name in ("t6-2.trace", "t6-3.trace"):
                self.assertEqual(session.check(session.from_log((Path(tmp) / name).read_text())), [])

    def test_a_fresh_name_is_used_as_given(self):
        with tempfile.TemporaryDirectory() as tmp:
            trace = Path(tmp) / "t2.trace"
            self.assertIn(f"trace written to {trace}", self._run_to(trace))
            self.assertTrue(trace.exists())


class Respond(unittest.TestCase):
    def test_respond_serves_a_status_read_and_stops_after_its_time(self):
        world = fake_world.World()
        out = io.StringIO()
        done = {}

        def serve():  # the ref board runs `respond`; the dut board asks it
            done["code"] = io2g4.main(["io2g4.py", "respond", "--port", "p", "--node", DEVICE.hex(), "--seconds", "0.5"],
                                      open_peer=lambda args: conformance.Peer(world.ref), environ={io2g4.KEY_ENV: KEY.hex()},
                                      out=out)
        # `respond` blocks until its time is up and advances the shared clock itself, so the asking board's
        # frames are put on the air first, at times inside that window.
        peer = conformance.Peer(world.dut)
        radio = benchkit.radio_of(benchkit.boot_ready(peer))
        benchkit.set_up(peer, radio, 0x56, bridle.CH20, listen=False, event_mask={})
        request = io2g4.status_request(DEVICE, CONTROLLER)
        world.schedule(world.now_us + 300_000, lambda: peer.send("tx", {
            "radio": 0, "freq_hz": bridle.CH20, "then_rx": True, "payload": io2g4.psdu_of(request).hex()}))
        serve()
        self.assertEqual(done["code"], io2g4.EXIT_OK)
        self.assertIn("answering as 516B10 on 20", out.getvalue().replace("2450000000", "20"))
        self.assertIn("status read from 4D84A6: answered 0x04", out.getvalue())
        self.assertRegex(out.getvalue(), r"status reply latency, end of the request -> start of the reply: \d+ us")
        self.assertEqual(io_commands_sent(world.ref), [protolib.CMD_PRIVATE_RESP])


if __name__ == "__main__":
    unittest.main()
