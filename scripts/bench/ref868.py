#!/usr/bin/env python3
"""Drive the 868 MHz bench reference radio (config/bench868-ref-v3.yaml) over the ESPHome native API.

    ref868.py tx --host H --freq HZ --preamble BYTES --hex HEX [--count N] [--seconds N]
    ref868.py carrier --freq HZ --ms MS [--seconds N]
    ref868.py tx-preamble-only --freq HZ --ms MS [--seconds N]
    ref868.py rx-listen --freq HZ --seconds N
    ref868.py rx-stop
    ref868.py logs --seconds N

Each command calls the matching action of the reference radio (or, for `log`, none), then collects the
device's log lines for `--seconds`, stamping each with the host's monotonic clock, and prints what the
bench needs: `rx` lines (frequency, length, bytes) and `bench868` lines (what the reference radio did
and when, by its own millis()). `--json` prints one JSON object per line. `--dry-run` prints the action
and its arguments and opens nothing. The API key is read from `--key` or the ESPHOME_API_KEY
environment variable. Runs in the ESPHome container (`docker compose run --rm --entrypoint python3
esphome scripts/bench/ref868.py ...`), which has `aioesphomeapi`; a bench frame carries a bench-only
source id (C0FFEE) and the scripts that read this output match on it.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import re
import sys
import time

API_PORT = 6053
ACTIONS = {"tx": "tx_frame", "rx-listen": "rx_listen", "rx-stop": "rx_stop", "carrier": "carrier",
           "tx-preamble-only": "tx_preamble_only"}
# `log_frame()` (log_frame.h): `RX [12 bytes] freq=868950000 cmd=...: HEX`, and the actions' own lines.
RX_LINE = re.compile(r"RX \[(?P<len>\d+) bytes\] freq=(?P<freq>\d+) (?:preamble=\d+ )?cmd=(?P<rest>.*?): (?P<hex>[0-9A-Fa-f ]+)$")
BENCH_LINE = re.compile(r"bench868(?::\d+)?\]?: (?P<text>.*)$")
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def action_arguments(args: argparse.Namespace) -> dict:
    """The arguments of the action `args.command` calls."""
    if args.command == "tx":
        return {"freq_hz": args.freq, "preamble": args.preamble, "hex": args.hex.replace(" ", "")}
    if args.command == "rx-listen":
        return {"freq_hz": args.freq}
    if args.command in ("carrier", "tx-preamble-only"):
        return {"freq_hz": args.freq, "ms": args.ms}
    return {}


def parse_line(message: str, host_s: float) -> dict | None:
    """One log line as a record, or None when it is neither a received frame nor a bench action line."""
    text = ANSI.sub("", message).strip()
    rx = RX_LINE.search(text)
    if rx:
        return {"kind": "rx", "host_s": host_s, "freq_hz": int(rx["freq"]), "length": int(rx["len"]),
                "hex": rx["hex"].replace(" ", "").upper()}
    bench = BENCH_LINE.search(text)
    if bench:
        return {"kind": "bench", "host_s": host_s, "text": bench["text"]}
    return None


async def run(host: str, key: str | None, args: argparse.Namespace, client_factory=None) -> list[dict]:
    """Call the action (if any), collect log records for `args.seconds`, and return them."""
    if client_factory is None:
        import aioesphomeapi

        def client_factory():
            return aioesphomeapi.APIClient(host, API_PORT, None, noise_psk=key)
    client = client_factory()
    records: list[dict] = []

    def on_log(entry) -> None:
        record = parse_line(entry.message.decode("utf-8", "replace") if isinstance(entry.message, bytes)
                            else entry.message, time.monotonic())
        if record:
            records.append(record)

    await client.connect(login=True)
    try:
        client.subscribe_logs(on_log, log_level=5)  # LOG_LEVEL_DEBUG
        if args.command in ACTIONS:
            _, services = await client.list_entities_services()
            service = next((s for s in services if s.name == ACTIONS[args.command]), None)
            if service is None:
                raise SystemExit(f"ref868: the device has no action {ACTIONS[args.command]!r}")
            for sent in range(getattr(args, "count", 1)):
                if sent:
                    await asyncio.sleep(args.gap_s)
                await client.execute_service(service, action_arguments(args))
        await asyncio.sleep(args.seconds)
    finally:
        await client.disconnect()
    return records


def render(records: list[dict], as_json: bool) -> str:
    lines = []
    for r in records:
        if as_json:
            lines.append(json.dumps(r))
        elif r["kind"] == "rx":
            lines.append(f"{r['host_s']:.3f} rx {r['freq_hz']} {r['length']} bytes {r['hex']}")
        else:
            lines.append(f"{r['host_s']:.3f} ref {r['text']}")
    return "\n".join(lines)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="ref868.py", description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    for name in (*ACTIONS, "logs"):
        p = sub.add_parser(name)
        p.add_argument("--host", required=True, help="the reference radio's address (its mDNS name or IP)")
        p.add_argument("--seconds", type=float, default=2.0, help="how long to collect log lines afterwards")
        p.add_argument("--key", default=os.environ.get("ESPHOME_API_KEY"), help="API encryption key (base64)")
        p.add_argument("--json", action="store_true")
        p.add_argument("--dry-run", action="store_true", help="print the action, open nothing")
    for name in ("tx", "rx-listen", "carrier", "tx-preamble-only"):
        sub.choices[name].add_argument("--freq", type=int, required=True, help="Hz")
    for name in ("carrier", "tx-preamble-only"):
        sub.choices[name].add_argument("--ms", type=int, required=True, help="how long the signal is on air")
    sub.choices["tx"].add_argument("--preamble", type=int, required=True, help="bytes")
    sub.choices["tx"].add_argument("--count", type=int, default=1, help="how many times to send the frame")
    sub.choices["tx"].add_argument("--gap-s", type=float, default=0.5, help="between two sends")
    sub.choices["tx"].add_argument("--hex", required=True, help="the io frame without CRC, hex")
    return parser


def main(argv: list[str], client_factory=None) -> int:
    args = build_parser().parse_args(argv[1:])
    if args.command == "tx" and (len(args.hex.replace(" ", "")) % 2 or not args.hex):
        raise SystemExit("ref868: --hex needs a whole number of bytes")
    if args.dry_run:
        print(json.dumps({"action": ACTIONS.get(args.command), "arguments": action_arguments(args),
                          "collect_seconds": args.seconds}))
        return 0
    records = asyncio.run(run(args.host, args.key, args, client_factory))
    text = render(records, args.json)
    if text:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
