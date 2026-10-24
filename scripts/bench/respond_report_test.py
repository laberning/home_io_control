#!/usr/bin/env python3
"""Self-test of scripts/bench/respond_report.py: against lines as `io2g4.py respond` prints them, and against
what a real stand-in prints in the in-process bench. Run via `make py-test`."""

from __future__ import annotations

import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import io2g4_test as bench  # noqa: E402  (also imports the Bridle package)
import respond_report  # noqa: E402

LOG = """answering as BE0D01 on 2450000000 Hz; Ctrl-C to stop
heard 0x00 from BE0001: CTRL1 0x20, after 509 wake frames
EXECUTE from BE0001: challenged
heard 0x3d from BE0001: CTRL1 0x20, after 0 wake frames
gap 0x3C end -> 0x3D start: 20500 us
answer from BE0001 is right: moved, answered 0x04
heard 0x00 from BE0001: CTRL1 0x20, after 8 wake frames
EXECUTE from BE0001: challenged
heard 0x3d from BE0001: CTRL1 0x20, after 0 wake frames
gap 0x3C end -> 0x3D start: 24500 us
answer from BE0001 is right: moved, answered 0x04
heard 0x03 from BE0001: CTRL1 0x20, after 510 wake frames
status read from BE0001: answered 0x04
heard 0x00 from BE0001: CTRL1 0x20, after 510 wake frames
EXECUTE from BE0001: challenged
heard 0x00 from BE0001: CTRL1 0x20, after 510 wake frames
EXECUTE from BE0001: challenged
heard 0x3d from BE0001: CTRL1 0x20, after 0 wake frames
gap 0x3C end -> 0x3D start: 31000 us
"""


class Report(unittest.TestCase):
    def test_every_execute_is_read_with_its_train_and_gap(self):
        report = respond_report.parse(LOG.splitlines())
        self.assertEqual([(e.train, e.request_ctrl1, e.answer_ctrl1, e.gap_us) for e in report.executes],
                         [("long", 0x20, 0x20, 20500), ("short", 0x20, 0x20, 24500),
                          ("long", 0x20, None, None), ("long", 0x20, 0x20, 31000)])
        self.assertEqual(report.gaps, [20500, 24500, 31000])

    def test_the_summary_has_the_minimum_median_and_maximum(self):
        text = respond_report.render(respond_report.parse(LOG.splitlines()))
        self.assertIn("4 EXECUTEs, 3 answered with a 0x3D", text)
        self.assertIn("gap: min 20500 us, median 24500 us, max 31000 us (n = 3)", text)
        self.assertIn("trains: 3 long, 1 short, 0 other", text)
        self.assertIn("CTRL1 of the 0x3D: 0x20", text)

    def test_a_log_without_an_execute_is_exit_status_1(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "r.log"
            path.write_text("answering as BE0D01\nstatus read from BE0001: answered 0x04\n")
            self.assertEqual(respond_report.main(["r", str(path)], io.StringIO()), 1)
            path.write_text(LOG)
            self.assertEqual(respond_report.main(["r", str(path)], io.StringIO()), 0)

    def test_it_reads_what_a_stand_in_prints(self):
        """The lines parsed here are the ones `Responder` writes: if its wording changes, this fails."""
        world = bench.fake_world.World()
        device = bench.stand_in(world)
        for argv in (("--wake", "short"), ("--lp-on-response",)):
            code, _, _, _ = bench._run(world, "execute", "--dst", bench.DEVICE.hex(), "--open", *argv,
                                       "--answer-delay-ms", "20", key_file_text=bench.KEY.hex())
            self.assertEqual(code, 0)
        report = respond_report.parse(device.out_text().splitlines())
        self.assertEqual([e.train for e in report.executes][0], "short")
        self.assertEqual([e.answer_ctrl1 for e in report.executes], [0x00, 0x20])
        self.assertEqual(len(report.gaps), 2)
        for gap in report.gaps:
            self.assertAlmostEqual(gap, 20_000, delta=50)


if __name__ == "__main__":
    unittest.main()
