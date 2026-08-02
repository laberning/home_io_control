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
  captured_with: sx1262                           # sx1276 | sx1262 | other | synthetic (radio chip)
  firmware: "2026.6 / commit f981dce"              # best effort, optional
  date: 2026-07-06
  origin: own-hardware                # own-hardware | github-issue | synthetic-bootstrap
  issue: null                          # e.g. "https://github.com/.../issues/42", else null
key: corpus            # corpus = HMAC/key-transfer bytes verify under the public corpus key
                       # unknown = raw community capture, crypto not verifiable
frames:
  - dir: tx                          # tx = controller->device, rx = device->controller
    t_ms: 0                          # relative timestamp in ms if known, else omit
    freq: 868950000                  # Hz; omit or 0 if unknown
    hex: "F6 20 96 D2 67 C0 FF EE 00 01 61 64 00 64 00 00 00 00 00 00 4C 8A"
    crc: present                     # present | absent — whether the trailing 2 bytes are the CRC
    note: "execute position 50 (raw 0x64), START"
  - dir: rx
    t_ms: 41
    freq: 868950000
    hex: "8E 10 C0 FF EE 96 D2 67 3C 01 23 45 67 89 AB 11 F0"
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
    - {cmd: 0x04, start: false, end: true, protocol: 2w}
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
- `source.*`: provenance metadata, enforced by `validate.py`. Required: `origin` (one of
  `own-hardware`, `github-issue`, `synthetic-bootstrap`), `captured_with` (one of `sx1276`,
  `sx1262`, `other`, `synthetic` — the **radio chip**, not the board model; `build.py` reads this
  unconditionally to pick a chip-mock in `corpus_exchange_replay_test.cpp`, which only branches
  on chip-specific timing behavior. Note the exact board/product if useful in
  `source.device` or `firmware` free text — e.g. "Heltec WiFi LoRa32 V3" — `captured_with` itself
  should not encode it), `device`, `date`. Optional: `firmware`, `issue` (set to `null` when
  there is no originating discussion, e.g. synthetic-bootstrap captures).
- `key` (required): `corpus` or `unknown`. See "Key hygiene" below — this is a promise the file
  makes, and `validate.py` enforces it cryptographically: every `key: corpus` capture's 0x3C/0x3D
  HMACs and 0x31/0x3C/0x32 key-transfer payloads must verify under the public corpus key.
- `node_map` (optional): role -> remapped node ID, only present if a capture's
  `--remap` was used at ingest. Node IDs are **not** treated as secret (see "Key hygiene" below)
  so most captures — own-hardware or community — keep the node IDs as captured and omit this
  field entirely.
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
  - `frames[].classification` (optional): a symbolic name from one of the `decisions::`
    disposition enums in `hub_decisions.h`, checked by `corpus_classification_test.cpp` against
    the pure classifier ExchangeEngine/PairingEngine actually call. Allowed names: `REQUIRE_AUTH`,
    `COMPLETE_DIRECT`, `IGNORE_UNRELATED`, `ACCEPT`, `NO_RESPONSE`, `INVALID`, `IGNORE`, and
    `DISCOVERY_ACCEPT` (`PairingDiscoveryDisposition::ACCEPT` specifically — disambiguated from
    the plain `ACCEPT` used by `ExchangeFinalResponseDisposition`/`PairingKeyChallengeDisposition`
    because the three enums don't share the same underlying numeric value for "accept"; see
    `scripts/corpus/build.py :: CLASSIFICATION_ENUM` for the full mapping). For `authenticated_command`
    / `direct` / `status_poll` exchanges, the first `rx` frame is classified via
    `classify_exchange_first_response()` and every `rx` frame after that via
    `classify_exchange_final_response()`, both against the origin `tx` frame — the same shape
    `ExchangeEngine::send_and_receive()` uses. For `pairing` captures, only a `CMD_DISCOVER_RESP`
    (0x29) frame can carry a `classification` expectation today (`classify_pairing_discovery_response()`
    needs only the frame itself); `classify_pairing_key_challenge()` additionally needs the
    discovered device's and controller's node IDs, which the schema doesn't carry yet.

### Raw bytes are immutable

Once a capture YAML is committed, its `hex` bytes are never hand-edited. Only
`scripts/corpus/ingest.py` (`--rekey` for re-key/anonymize) may rewrite bytes, and only before
the first commit of that capture. `expect:` values may be corrected by hand with a commit
message explaining why (e.g. the code's prior behavior was itself the bug).

### Key hygiene — ⚠️ read before ever pasting a pairing log

- The real system key must never appear in any committed file, generated file, test, log
  output, or step report.
- `key: corpus` promises the capture's HMAC / key-transfer bytes verify under the public
  corpus key (`tests/support/test_helpers.h::TEST_SYSTEM_KEY`) — never the real key.
- **Pairing captures (containing a `0x32` key-transfer frame) leak the real system key** if
  committed as captured — the transfer key is public and hardcoded, so anyone with the raw
  bytes can recover the real key. Pairing captures should be re-keyed before they are ever
  committed, and raw pairing logs should never be pasted into a public GitHub issue.
- **Only the real system key is secret.** Node IDs — controller ID, device IDs, remote/sensor
  IDs — are not: they're routing addresses on an already-broadcast RF protocol, not credentials,
  and knowing one gives no path to a device or its key. Captures (own-hardware **and**
  community) keep node IDs exactly as captured by default; `ingest.py --remap`/`--role` exist
  but are optional and not the recommended default. `key: unknown` community captures need no
  cryptographic anonymization either — HMAC bytes without the key are not attackable. Keeping
  node IDs as captured also makes a capture directly cross-referenceable against its source (a
  linked GitHub thread, or your own device labels).

### Expectations are human-verified

`expect:` fields are not auto-derived and bulk-accepted from current code output — that would
just enshrine today's behavior, bugs included. A human (or an AI agent, with explicit
per-capture review reasoning recorded in the change) confirms each expectation before commit.

### Crypto enforcement

`validate.py` recomputes and verifies crypto for every capture, independent of what its `key:`
field claims:
- `key: corpus` captures: every 0x3C/0x3D HMAC must verify, and every 0x31/0x3C/0x32
  key-transfer payload must decrypt, under the public corpus key
  (`scripts/corpus/protolib.py :: CORPUS_SYSTEM_KEY`, mirroring
  `tests/support/test_helpers.h :: TEST_SYSTEM_KEY`).
- **Any** capture, regardless of `key:` mode, containing a 0x32 key-transfer frame whose
  payload does not decrypt to the corpus key is a hard validation failure — this is the safety
  net that stops an un-re-keyed raw pairing capture from ever being committed.

Cross-language agreement between this Python port and the real C++ implementation is pinned by
known-answer vectors: `scripts/corpus/tests/data/crypto_kat.yaml` and the hardcoded vectors in
`tests/corpus_crypto_test.cpp` are both generated from the same C++ run
(`tests/corpus_bootstrap_dump_test.cpp :: DISABLED_PrintCryptoKatVectors`); a divergence between
the two implementations fails a gate on both sides.

## Contribution workflow

1. Capture frames — own hardware, or a community-supplied log via `IOHOME_FRAME_LOG`/monitor
   configs (see `docs/radio_diagnostics.md` and the README's
   [Reporting Unsupported Devices](../../README.md#reporting-unsupported-devices) checklist).
   ⚠️ Never paste a raw pairing log (`0x31`/`0x32`/`0x33`) publicly — see "Key hygiene" above.

   **Getting the raw `0x32` bytes for `--rekey`:** a normal build cannot produce them — `0x32`
   payloads are always masked in frame logs (`log_frame.h`'s `render_frame_hex_redacted()`),
   independent of `IOHOME_FRAME_LOG`. To capture a genuine own-hardware pairing exchange for
   re-keying, temporarily rebuild with the separate, maintainer-only
   `-DIOHOME_UNSAFE_LOG_KEY_MATERIAL` flag (see its doxygen in `log_frame.h` for the full rules),
   capture the one exchange you need, then rebuild without it immediately. Never commit a config
   with it enabled, never use it on a device whose logs anyone else can see, and never paste
   output captured under it anywhere before `--rekey` has run. (If the raw bytes genuinely cannot
   be obtained at all and you independently know the plaintext inputs — a real recovered key, the
   challenge, and the IV `data` convention — the corpus-key payload can instead be computed
   directly via `crypt_key()`/`protolib.crypt_key()` without ever touching the real key bytes;
   label such a frame's `note:` clearly as computed, not captured, if you ever fall back to this.)
2. Scaffold a capture YAML with `scripts/corpus/ingest.py`, which parses both on-air log tags
   (`io_capture` structured, legacy `io_frame`) plus a liberal fallback for mangled pastes, and
   proposes mechanically-derivable `expect:` fields (cmd/start/end/protocol only):
   ```bash
   python3 scripts/corpus/ingest.py analysis/issues/27.txt \
       --id issue_27_somfy_sunea_discovery --device "Somfy Sunea IO motor" \
       --captured-with sx1262 --origin github-issue \
       --issue https://github.com/laberning/home_io_control/issues/27 --date 2026-07-06 \
       -o tests/corpus/captures/issues/issue_27_somfy_sunea_discovery.yaml
   ```
   Pipe a trimmed excerpt through stdin (`sed -n '10,40p' file.txt | ingest.py - ...`) instead of
   ingesting a whole log when you only want one scenario out of it. Every capture `ingest.py`
   emits is `key: unknown` unless you pass `--rekey`.
3. **Own-hardware captures with a real system key**: re-key at ingest time instead of committing
   real crypto. `--rekey` verifies every captured HMAC / key-transfer payload against your real
   key (hard abort on any mismatch — never silently produces bad output), rewrites them under
   the public corpus key, and marks `key: corpus`. Node IDs are kept as captured — see "Key
   hygiene" above, only the key itself needs this treatment:
   ```bash
   python3 scripts/corpus/ingest.py my_capture.log --rekey \
       --system-key-from config/secrets.yaml \
       --id somfy_awning_exchange_open --device "..." --captured-with sx1262 \
       --origin own-hardware --date 2026-07-06 \
       -o tests/corpus/captures/somfy_awning/exchange_open.yaml
   ```
   `--system-key-from` refuses to read a path that is not git-ignored. Run this only locally,
   never in CI, and never on a community-supplied log you don't have the real key for.
   `--remap OLDHEX=NEWHEX`/`--role NEWHEX=name` are available if you ever want to anonymize a
   specific node ID for some other reason, but are not needed for privacy.
4. Fill/correct `expect:` only with fields you have verified — `ingest.py`'s proposals are
   marked `# PROPOSED — verify before commit` and must be confirmed against real decoded output
   (own hardware) or the issue thread's established facts (community logs), not rubber-stamped.
5. Run `python3 scripts/corpus/validate.py` locally (or `make corpus-validate`, part of `make lint`)
   to check the capture is internally self-consistent (CRC, CTRL0 length, schema) and that its
   crypto promises hold (`key: corpus` HMACs verify; any `0x32` frame, in any capture, decrypts
   to the corpus key — see "Crypto enforcement" above).
6. Commit the capture YAML. That's it — `make unit-test` regenerates the C++ fixture header
   from it automatically; there is no generated file to commit alongside it.

### Growth convention

A device-specific protocol bug is not considered fixed until its capture is in the corpus: the
fixing PR should add the capture (and a decode/classification assertion that would have caught
the bug) in the same change. Issue-derived captures live under `captures/issues/`, named
`issue_<n>_<slug>.yaml`, with `source.issue` pointing at the originating discussion — so every
fixture stays traceable to the report that motivated it.
