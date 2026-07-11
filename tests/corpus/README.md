# Golden-Frame Corpus — Format Spec

A versioned corpus of real (and a few synthetic bootstrap) IO-Homecontrol frame captures,
used as regression fixtures for the host test suite. Every capture is a fixture: an issue log
becomes a permanent parser regression test the day it's ingested, and a device-specific
protocol bug is not considered fixed until its capture is in the corpus. This file is the
normative format spec — read it before adding or editing a capture.

## Directory layout

```
tests/corpus/
  README.md                # this file
  captures/
    _bootstrap/              # synthetic pipeline self-test fixtures, kept permanently
    <device_family>/          # one directory per device/source family (e.g. somfy_awning)
    issues/                   # community captures from GitHub issues
```

The C++ fixture header (`build/corpus/corpus_generated.h`) is a **build artifact**, generated
by `scripts/corpus/build.py` automatically as part of `make unit-test` and git-ignored — never
hand-edit it, and there is nothing to commit or keep in sync: the YAML captures are the single
source of truth, so there is no second copy of the data that can drift.

One directory per device/source family, one YAML file per scenario. Issue-derived captures
live under `captures/issues/` named `issue_<n>_<slug>.yaml` so each file is traceable to its
origin discussion.

## Capture file schema

```yaml
id: somfy_awning_exchange_position_50   # unique across the whole corpus; kebab/snake case
description: >
  Free-text summary of the scenario.
source:
  device: "Somfy awning actuator (io Vertical)"   # free text, as much as known
  captured_with: heltec-v3                        # heltec-v2 | heltec-v3 | other | synthetic
  firmware: "2026.6 / commit f981dce"              # best effort, optional
  date: 2026-07-06
  origin: own-hardware                # own-hardware | github-issue | synthetic-bootstrap
  issue: null                          # e.g. "https://github.com/.../issues/42", else null
key: corpus            # corpus = HMAC/key-transfer bytes verify under the public corpus key
                       # unknown = raw community capture, crypto not verifiable
node_map:               # anonymization record: role -> ID as stored in the frames below
  controller: "AAAA01"
  awning:     "BBBB01"
frames:
  - dir: tx                          # tx = controller->device, rx = device->controller
    t_ms: 0                          # relative timestamp in ms if known, else omit
    freq: 868950000                  # Hz; omit or 0 if unknown
    hex: "F6 20 BB BB 01 AA AA 01 00 01 61 64 00 64 00 00 00 00 00 00 4C 8A"
    crc: present                     # present | absent — whether the trailing 2 bytes are the CRC
    note: "execute position 50 (raw 0x64), START"
  - dir: rx
    t_ms: 41
    freq: 868950000
    hex: "8E 10 AA AA 01 BB BB 01 3C 01 23 45 67 89 AB 11 F0"
    crc: present
    note: "challenge request, challenge = 01 23 45 67 89 AB"
expect:                              # deliberately sparse — only assert what a human verified
  exchange:
    kind: authenticated_command       # direct | authenticated_command | pairing | oneway | status_poll
    outcome: success                  # success | timeout | failure
  frames:
    - {cmd: 0x00, start: true, end: false, protocol: 2w}
    - {cmd: 0x3C, classification: REQUIRE_AUTH}
    - {cmd: 0x3D, hmac_valid: true}
    - {cmd: 0x04, end: true}
  device:
    reported_position: 50
    name: "Terrace Awning"
  oneway:
    intent: CLOSE
    target_type: 2
    originator: 0x01
    acei: 2
```

### Field reference

- `id` (string, required): globally unique. `validate.py`/`build.py` hard-fail on duplicates.
- `description` (string, required): what the scenario is and why it's here.
- `source.*`: provenance metadata. `origin` is required and one of `own-hardware`,
  `github-issue`, `synthetic-bootstrap`. `issue` is required (may be `null`) so the field is
  always present for `issue`-origin captures.
- `key` (required): `corpus` or `unknown`. See "Key hygiene" below — this is a promise the
  file makes, and `validate.py` will eventually enforce it cryptographically (crypto
  verification lands in a later tool version; see "Current limitations").
- `node_map` (optional): role -> node-ID-as-captured. Documents the anonymization applied at
  ingest; omit for synthetic fixtures with no real device to anonymize.
- `frames` (required, at least 1): every frame is a fixture even with zero `expect` entries —
  the universal wire invariants (round-trip, CRC, CTRL0 length) apply to all of them.
  - `dir`: `tx` (controller->device) or `rx` (device->controller).
  - `t_ms` (optional): relative timestamp in milliseconds.
  - `freq` (optional): capture frequency in Hz; omit or `0` if unknown.
  - `hex` (required): the exact wire bytes as captured, space-separated hex, even number of
    hex digits. Includes the trailing CRC bytes when `crc: present`.
  - `crc`: `present` or `absent` — whether the trailing 2 bytes of `hex` are the CRC.
  - `note` (optional): human annotation.
- `expect` (optional): sparse, per-field-optional assertions. Nothing here is required; add
  only what a human has verified against real behavior — see "Expectations are
  human-verified" below.

### Raw bytes are immutable

Once a capture YAML is committed, its `hex` bytes are never hand-edited. Only
`scripts/corpus/ingest.py` (re-key/anonymize, arrives in a later tool version) may rewrite
bytes, and only before the first commit of that capture. `expect:` values may be corrected by
hand with a commit message explaining why (e.g. the code's prior behavior was itself the bug).

### Key hygiene — ⚠️ read before ever pasting a pairing log

- The real system key must never appear in any committed file, generated file, test, log
  output, or step report.
- `key: corpus` promises the capture's HMAC / key-transfer bytes verify under the public
  corpus key (`tests/support/test_helpers.h::TEST_SYSTEM_KEY`) — never the real key.
- **Pairing captures (containing a `0x32` key-transfer frame) leak the real system key** if
  committed as captured — the transfer key is public and hardcoded, so anyone with the raw
  bytes can recover the real key. Pairing captures must be re-keyed before they are ever
  committed, and raw pairing logs must never be pasted into a public GitHub issue.
- `key: unknown` community captures need no cryptographic anonymization — HMAC bytes without
  the key are not attackable — but node IDs should still be remapped via `node_map` as a
  privacy courtesy.

### Expectations are human-verified

`expect:` fields are not auto-derived and bulk-accepted from current code output — that would
just enshrine today's behavior, bugs included. A human (or an AI agent, with explicit
per-capture review reasoning recorded in the change) confirms each expectation before commit.

### Current limitations

The `key: corpus` cryptographic promise (recomputing HMACs / key-transfer payloads against the
corpus key) is **not yet enforced** by `validate.py` — that requires a Python AES/HMAC port
and lands in a later tool version. Until then, `validate.py` emits a labeled `SKIP` line for
this check so the gap stays visible rather than silently passing.

## Contribution workflow

1. Capture frames (own hardware, or via `IOHOME_FRAME_LOG`/monitor configs — see
   `docs/radio_diagnostics.md`).
2. Scaffold a capture YAML per this schema (an `ingest.py` scaffolding tool arrives in a later
   tool version; today, copy an existing capture as a template).
3. Fill `expect:` only with fields you have verified against real decoded/classified output.
4. Run `python3 scripts/corpus/validate.py` locally (or `make corpus-validate`, part of `make lint`)
   to check the capture is internally self-consistent (CRC, CTRL0 length, schema).
5. Commit the capture YAML. That's it — `make unit-test` regenerates the C++ fixture header
   from it automatically; there is no generated file to commit alongside it.
