#!/usr/bin/env python3
"""Self-test of scripts/bench/ref868.py against a fake API client. Run via `make py-test` (in the
ESPHome container; the test needs no `aioesphomeapi` and no network)."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import unittest
from pathlib import Path
from types import SimpleNamespace

_spec = importlib.util.spec_from_file_location("ref868", Path(__file__).resolve().parent / "ref868.py")
ref868 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ref868)

RX = "\x1b[0;32m[I][io_frame:106]: RX [12 bytes] freq=868950000 cmd=EXECUTE ctrl1=0x00: 05 40 91 15 55 33 51 12 25 99 00 50"
RX_PREAMBLE = "[I][io_frame:103]: RX [9 bytes] freq=868250000 preamble=48 cmd=STATUS : C8 00 00 00 00 00 00 00 00"
BENCH = "[I][bench868:75]: TX freq=868950000 preamble=1024 len=12 at_ms=4711 result=OK"


class FakeClient:
    """Stands in for aioesphomeapi.APIClient: records what is asked and replays log lines."""

    instances: list["FakeClient"] = []

    def __init__(self, lines=(), actions=("tx_frame", "rx_listen", "rx_stop")):
        self.lines, self.actions, self.called, self.disconnected = list(lines), actions, [], False
        FakeClient.instances.append(self)

    async def connect(self, login=False):
        self.logged_in = login

    def subscribe_logs(self, callback, log_level=None):
        for line in self.lines:
            callback(SimpleNamespace(message=line.encode()))

    async def list_entities_services(self):
        return [], [SimpleNamespace(name=name) for name in self.actions]

    async def execute_service(self, service, data):
        self.called.append((service.name, data))

    async def disconnect(self):
        self.disconnected = True


def run_main(*argv, client=None):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = ref868.main(["ref868.py", *argv, "--host", "ref.local"], (lambda: client) if client else None)
    return code, out.getvalue()


class Parsing(unittest.TestCase):
    def test_a_received_frame_line_gives_frequency_length_and_bytes(self):
        record = ref868.parse_line(RX, 1.5)
        self.assertEqual(record, {"kind": "rx", "host_s": 1.5, "freq_hz": 868950000, "length": 12,
                                  "hex": "054091155533511225990050"})

    def test_a_line_with_a_preamble_field_parses_too(self):
        self.assertEqual(ref868.parse_line(RX_PREAMBLE, 0)["freq_hz"], 868250000)

    def test_an_action_line_is_kept_as_text(self):
        record = ref868.parse_line(BENCH, 2.0)
        self.assertEqual(record["kind"], "bench")
        self.assertIn("TX freq=868950000 preamble=1024", record["text"])

    def test_other_lines_are_dropped(self):
        self.assertIsNone(ref868.parse_line("[D][wifi:100]: RSSI -50", 0))


class Commands(unittest.TestCase):
    def setUp(self):
        FakeClient.instances.clear()

    def test_tx_frame_calls_the_action_with_its_arguments_and_prints_what_was_logged(self):
        client = FakeClient([BENCH, RX])
        code, out = run_main("tx", "--freq", "868950000", "--preamble", "1024",
                             "--hex", "05 40 91 15", "--seconds", "0", client=client)
        self.assertEqual(code, 0)
        self.assertEqual(client.called, [("tx_frame", {"freq_hz": 868950000, "preamble": 1024, "hex": "05409115"})])
        self.assertTrue(client.disconnected)
        self.assertIn(" rx 868950000 12 bytes 054091155533511225990050", out)
        self.assertIn(" ref TX freq=868950000", out)

    def test_rx_listen_json_output_is_one_object_per_line(self):
        client = FakeClient([RX, RX_PREAMBLE])
        _, out = run_main("rx-listen", "--freq", "868950000", "--seconds", "0", "--json", client=client)
        records = [json.loads(line) for line in out.splitlines()]
        self.assertEqual([r["freq_hz"] for r in records], [868950000, 868250000])
        self.assertEqual(client.called, [("rx_listen", {"freq_hz": 868950000})])

    def test_the_test_signals_call_their_actions_with_a_duration(self):
        for command, action in (("carrier", "carrier"), ("tx-preamble-only", "tx_preamble_only")):
            client = FakeClient(actions=("carrier", "tx_preamble_only"))
            run_main(command, "--freq", "868950000", "--ms", "300", "--seconds", "0", client=client)
            self.assertEqual(client.called, [(action, {"freq_hz": 868950000, "ms": 300})])

    def test_tx_count_sends_the_frame_that_many_times(self):
        client = FakeClient()
        run_main("tx", "--freq", "868950000", "--preamble", "48", "--hex", "0102", "--count", "3", "--gap-s", "0",
                 "--seconds", "0", client=client)
        self.assertEqual([name for name, _ in client.called], ["tx_frame"] * 3)

    def test_log_only_calls_no_action(self):
        client = FakeClient([RX])
        run_main("logs", "--seconds", "0", client=client)
        self.assertEqual(client.called, [])

    def test_a_device_without_the_action_is_an_error(self):
        client = FakeClient(actions=("rx_stop",))
        with self.assertRaises(SystemExit) as stopped:
            run_main("rx-listen", "--freq", "1", "--seconds", "0", client=client)
        self.assertIn("no action 'rx_listen'", str(stopped.exception))
        self.assertTrue(client.disconnected)


class DryRun(unittest.TestCase):
    def test_dry_run_prints_the_action_and_opens_nothing(self):
        FakeClient.instances.clear()
        for argv in (["tx", "--freq", "868950000", "--preamble", "8", "--hex", "0102"],
                     ["rx-listen", "--freq", "868250000"], ["rx-stop"], ["logs"],
                     ["carrier", "--freq", "868950000", "--ms", "100"],
                     ["tx-preamble-only", "--freq", "868950000", "--ms", "100"]):
            code, out = run_main(*argv, "--dry-run")
            self.assertEqual(code, 0)
            self.assertIn("action", json.loads(out))
        self.assertEqual(FakeClient.instances, [])

    def test_an_odd_hex_string_is_refused(self):
        with self.assertRaises(SystemExit):
            run_main("tx", "--freq", "1", "--preamble", "8", "--hex", "012", "--dry-run")


if __name__ == "__main__":
    unittest.main()
