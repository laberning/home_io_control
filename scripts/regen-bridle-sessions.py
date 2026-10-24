#!/usr/bin/env python3
"""Regenerate the Bridle session fixtures in tests/corpus/sessions/ from the tests that record them.

Each fixture is the trace one host test writes while it drives the hub over a scripted
co-processor (HIOC_BRIDLE_TRACE_DIR, tests/support/testable_radio_bridle.h), turned into a
recording by ``bridle-session from-log`` with origin ``synthetic-bootstrap``. They
pin the driver's own command sequence: nothing here shows what a real co-processor does.

A regenerated fixture passes its replay by construction, so review the diff of every file this
changes before committing it. Regenerate when the driver's commands change on purpose, or when the
protocol version moves (a recording carries the version it was made with).

Run via ``make bridle-sessions-regen``, which builds the unit-test binary first.
"""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BINARY = REPO / "build" / "host" / "default" / "test_home_io_control"
OUT_DIR = REPO / "tests" / "corpus" / "sessions"

# (fixture id, the test that records it, what the conversation shows)
SESSIONS = (
    (
        "bridle_exchange_execute_challenge",
        "ExchangeOverBridle.ExecuteWithChallengeIsAnsweredAndTheStatusReplyEndsIt",
        "Boot handshake, configuration, then an EXECUTE to an always-alive device: the challenge, the "
        "answer to it and the status reply, with no wake-up strobe.",
    ),
    (
        "bridle_exchange_long_strobe",
        "ExchangeOverBridle.TheStartFrameToALowPowerDeviceGetsTheLongTrainAndItsContinuationsNone",
        "The same exchange to a low-power device: the START frame goes out behind a 510-frame strobe "
        "(about half a second on air), the challenge answer as a plain frame.",
    ),
    (
        "bridle_868_boot_configure_dual_band",
        "RadioBridle868.TheBootHandshakeWithADualBandCoprocessorConfiguresItsGfskRadio",
        "Boot handshake with the dual-band co-processor (802.15.4 as radio 0, gfsk_raw as radio 1) and the "
        "configuration of the 868 MHz radio: profile, modem, sync word, rx_len, detector, sync timeout.",
    ),
    (
        "bridle_868_exchange_execute_challenge",
        "ExchangeOverBridle868.ExecuteWithChallengeIsAnsweredAndTheStatusReplyEndsIt",
        "An EXECUTE to an always-alive device over the 868 radio: the challenge, the answer to it and the "
        "status reply, the replies reported rx_len bytes behind their sync word.",
    ),
    (
        "bridle_868_exchange_long_preamble",
        "ExchangeOverBridle868.TheStartFrameToALowPowerDeviceGoesOutBehindTheLongPreambleOnly",
        "The same exchange to a low-power device: the START frame goes out behind a 1024-byte preamble "
        "(about 214 ms on air) with no strobe, the challenge answer behind the short one.",
    ),
    (
        "bridle_link_unsolicited_ready",
        "RadioBridleLink.AnUnsolicitedReadyReconfiguresWithoutAnotherResetPulse",
        "The co-processor reboots in the middle of a session: an unsolicited ready with reset cause "
        "watchdog, and the driver configures the event mask and the radio again.",
    ),
)


def main() -> int:
    if not BINARY.is_file():
        print(f"error: {BINARY} is missing; run `make bridle-sessions-regen`, which builds it", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, HIOC_BRIDLE_TRACE_DIR=tmp)
        subprocess.run(
            [str(BINARY), "--gtest_filter=" + ":".join(test for _, test, _ in SESSIONS)],
            check=True, env=env, stdout=subprocess.DEVNULL,
        )
        OUT_DIR.mkdir(parents=True, exist_ok=True)
        for fixture, test, description in SESSIONS:
            trace = Path(tmp) / (test + ".log")
            if not trace.is_file():
                print(f"error: {test} wrote no trace", file=sys.stderr)
                return 1
            subprocess.run(
                [sys.executable, "-m", "bridle.session", "from-log", str(trace), "-o", str(OUT_DIR / f"{fixture}.yaml"),
                 "--source-origin", "synthetic-bootstrap", "--source-recorded-by", f"host test {test}",
                 "--source-description", description],
                check=True,
            )
            print(f"wrote {OUT_DIR / (fixture + '.yaml')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
