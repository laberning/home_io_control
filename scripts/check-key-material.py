#!/usr/bin/env python3
"""Scans tracked prose/doc-shaped and source files for accidentally-committed, byte-exact key
material.

`redaction.h::command_carries_key_material()` stops *our own firmware* from logging fresh
CMD_ONEWAY_ADD_CONTROLLER (0x30) / CMD_KEY_TRANSFER (0x32) / CMD_CHALLENGE_REQ (0x3C) /
CMD_CHALLENGE_RESP (0x3D) payloads. It does nothing about a human hand-pasting a genuine
captured pairing exchange into a doc, a debugging comment, an issue-investigation note, a corpus
YAML, or a test fixture, and committing it. This script is that backstop: it walks tracked
`*.md`/`*.txt`/`*.yaml`/`*.yml`/`*.cpp`/`*.h`/`*.hpp` files, extracts hex- and base64-looking byte
runs from their text, and flags any run that decodes to a CRC-valid frame (or, for the two
key-carrying opcodes only, a CRC-less run whose shape exactly matches this project's own
un-CRC'd log-paste format) at one of the four watched opcodes.

Scope: `git ls-files -- '*.md' '*.txt' '*.yaml' '*.yml' '*.cpp' '*.h' '*.hpp'`, excluding
everything under `tests/corpus/captures/` -- that directory already has a strictly stronger,
structurally-aware gate (scripts/corpus/validate.py, wired into `make lint` as
`corpus-validate`), which cryptographically verifies every `key: corpus` capture's HMACs and
key-transfer payloads against the known corpus key. Re-scanning that directory with this flat,
context-free text scanner would be a redundant, weaker check layered on top of a better one that
already runs.

Source is in scope, production and `tests/` alike: a human can paste a real captured frame into a
debugging comment as easily as into a doc, and a real key can end up as "test data" by accident
as easily as a synthetic one gets hand-built on purpose. An earlier version of this scanner
excluded `tests/**/*.cpp` outright, on the theory that hand-built watched-opcode byte-array
literals there would drown a CRC-valid check in false positives. Verified empirically
(2026-08-28) against all 213 tracked `*.cpp`/`*.h`/`*.hpp` files at the time: zero findings. This
codebase's tests build frames via protocol-builder helper functions or load them from
`tests/corpus/captures/` fixtures at runtime rather than embedding full valid-CRC frames as hex
literals in source text, so the theoretical noise never actually materializes. If a future
hand-built literal ever does trip this, the fix is to build it the way the rest of the codebase
already does -- a helper function or a corpus fixture -- not to add the exclusion back.

`reference/` and `analysis/` are both listed in `.git/info/exclude`, so `git ls-files` never
sees either directory -- this scanner has no dependency on and does not consult either one.

Policy: no allowlist. Any frame matching either detection tier below, in an in-scope file, is a
hard failure, unconditionally. There is no legitimate reason for prose documentation to embed a
byte-exact, CRC-valid key-transfer or challenge-response frame -- the fix for a real hit is
always to replace it with prose, a `note:`-only reference to the corpus fixture that already
holds the real bytes, or a deliberately-broken example (wrong CRC byte) if a hex illustration is
genuinely needed. An allowlist would exist to make a real hit pass without changing the file,
which is exactly the failure mode a security backstop must not have.

Detection has two tiers:

1. CRC-valid (the primary signal): at every byte offset in an extracted candidate run, if the
   command byte is one of the four watched opcodes, the CTRL0-declared length is in range, and
   the two bytes immediately following the candidate frame form a little-endian CRC-16 matching
   `protolib.crc_ccitt()` over the candidate -- that is a finding. AES ciphertext, ASCII text and
   ordinary command bytes essentially never validate a CRC by coincidence, so this is a real
   detector rather than a plain opcode grep. Ported from this project's own
   `radio_soft_phy.cpp::find_crc_valid_frame()`, minus its UART-specific surrounding code.
2. CRC-less key-wrap shape (narrow, hex-run-only, two opcodes only): every on-air log line this
   firmware emits (`io_capture`/`io_frame`, see `protolib.RawFrame.crc_present()`) logs frame
   bytes *without* the trailing CRC, so a CRC-only detector would miss the single most likely
   leak shape -- a human pasting a captured log line straight into a doc. This tier fires with no
   CRC required when a whole hex run (offset 0 to the run's own end -- a pasted single frame, not
   byte soup) is exactly a CMD_KEY_TRANSFER frame at its full 25-byte key-wrap length, or exactly
   a CMD_ONEWAY_ADD_CONTROLLER frame at its 29-byte declared length (optionally plus its 6-byte
   MAC trailer). CMD_CHALLENGE_REQ/RESP are excluded from this tier: their payload is 6 generic
   HMAC bytes, far too weak a shape to assert on without a CRC.

Not a complete detector: it catches byte-exact frames in prose-shaped tracked text, not a bare
16-byte key with no frame around it, a paraphrased/partially-transcribed capture, a screenshot,
or anything in a file type outside the scope above. A general "flag any bare key-shaped hex run"
tier was deliberately not built: this repo already has several legitimate high-entropy 16-byte
values with no frame around them (a test system key in tests/hub_core_test.cpp, a published IV
vector in proto_crypto.cpp, firmware hashes in lr1121_firmware_update_controller.cpp) that are
structurally identical to a real leaked key -- telling them apart needs an allowlist, which is
the exact failure mode this scanner's no-allowlist policy exists to avoid.

A second, independent check narrows that gap for the one case where "real key" and "any 16 random
bytes" *are* distinguishable: config/secrets.yaml (gitignored, local-only -- never present in CI
or on a fresh clone) holds this developer's actual live secrets. If that file exists,
`_load_local_secret_key_values()` reads every field whose name contains "key" (case-insensitive
-- `hub_system_key`, `api_key`; deliberately not every field, since short non-key values like
`hub_node_id` coincide with joke placeholders already used deliberately in this repo's test
fixtures, e.g. `C0FFEE`) and `scan_for_local_secret_leaks()` greps every tracked file for that
literal value. This is a denylist of known-real secrets, not a shape heuristic, so it sidesteps
the ambiguity above entirely -- and because CI never has the file, it silently checks 0 secrets
there and only bites locally, exactly where a real leak could happen.
"""

import base64
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parent.parent
SCRIPTS_DIR = REPO_ROOT / "scripts" / "corpus"
SECRETS_FILE = REPO_ROOT / "config" / "secrets.yaml"
sys.path.insert(0, str(SCRIPTS_DIR))
import protolib  # noqa: E402

# --- Local constants -- not in protolib.py, which is scoped to what the corpus toolchain needs.
FRAME_CMD_OFFSET = 8  # CTRL0(1)+CTRL1(1)+DST(3)+SRC(3) -- proto_sizes.h layout.
FRAME_CRC_SIZE = 2  # proto_sizes.h :: FRAME_CRC_SIZE.

# Watch list is pinned to redaction.h::command_carries_key_material(); keep the two in sync.
WATCHED_OPCODES = (
    protolib.CMD_ONEWAY_ADD_CONTROLLER,
    protolib.CMD_KEY_TRANSFER,
    protolib.CMD_CHALLENGE_REQ,
    protolib.CMD_CHALLENGE_RESP,
)

# Declared-length shapes for the CRC-less tier's two eligible opcodes -- the two that actually
# carry a wrapped key.
KEY_TRANSFER_DECLARED_LEN = protolib.FRAME_MIN_SIZE + protolib.AES_KEY_SIZE  # 25
ONEWAY_ADD_CONTROLLER_DECLARED_LEN = (
    protolib.FRAME_MIN_SIZE + protolib.ONEWAY_ADD_CONTROLLER_DECLARED_PAYLOAD_SIZE
)  # 29

CORPUS_CAPTURES_PREFIX = "tests/corpus/captures/"
SCAN_PATTERNS = ("*.md", "*.txt", "*.yaml", "*.yml", "*.cpp", "*.h", "*.hpp")

# --- Candidate byte-run extraction --------------------------------------------------------------
# A separate regex from protolib.parse_hex() by design: that function converts an already
# isolated, whitespace-only hex string, while this one's job is finding hex-shaped substrings
# inside arbitrary prose (0x/\x-prefixed or comma/colon/hyphen/underscore-separated). Once a
# candidate run's decorations are stripped, the actual hex-to-bytes conversion is handed to
# protolib.parse_hex() rather than hand-rolling a second one.
#
# Anchoring: protolib._HEX_RUN_RE solves the general "don't absorb an adjacent non-hex-run token"
# problem with \b word-boundary anchors, but its separator is whitespace-only, so \b (a transition
# between \w and non-\w) is a correct boundary test there. This scanner's separator class also
# includes `_`, which *is* a \w character -- a plain \b anchor would reject the legitimate
# underscore-separated shape "AB_CD" (no \w/non-\w transition between "B" and "_"). Anchor on
# adjacency to another hex digit instead: a token may not be immediately preceded or followed by
# a further [0-9A-Fa-f] character. That still blocks the failure case this exists for -- a decimal
# run like "preamble=1024" contributing a spurious leading "24" byte to an adjacent real hex run
# (every 2-char substring of "1024" has a hex-digit neighbor on at least one side, so none match)
# -- while still permitting "AB_CD", "AB-CD", "AB:CD" etc., since none of `_`, `-`, `:` are hex
# digits.
_HEX_TOKEN = r"(?<![0-9A-Fa-f])(?:0x|\\x)?[0-9A-Fa-f]{2}(?![0-9A-Fa-f])"
# Separators must not cross a newline: no real capture format in this project emits a single frame
# across a line break, so a hex run is confined to one line. `\s` (used by protolib's whitespace
# tier) is deliberately not reused here -- it matches `\n`, which would let two separate pasted
# frame lines in a markdown fence merge into a single decoded run.
_HEX_SEP = r"[ \t,:_-]+"
_HEX_RUN_RE = re.compile(_HEX_TOKEN + r"(?:" + _HEX_SEP + _HEX_TOKEN + r")*")
_HEX_TOKEN_RE = re.compile(_HEX_TOKEN)

# Base64 runs: speculative coverage for a third-party paste (nothing in this project emits
# base64 frame bytes). Safe to keep broad because the CRC gate below makes an accidental match on
# a hex SHA, an identifier, or a URL -- all valid base64 alphabet -- vanishingly unlikely. The
# CRC-less shape tier is hex-run-only for the opposite reason: a CRC-less base64 match would have
# no corroborating signal at all.
_BASE64_RUN_RE = re.compile(r"[A-Za-z0-9+/]{16,}={0,2}")

REPLACEMENT_CHAR = "�"
REPLACEMENT_WINDOW = 32


@dataclass
class Finding:
    path: Path
    line: int
    tier: str  # "crc-valid" | "shape"
    cmd: int
    declared_len: int
    candidate_len: int
    raw_prefix: bytes
    replacement_nearby: bool

    def describe(self) -> str:
        rel = self.path.relative_to(REPO_ROOT) if self.path.is_absolute() else self.path
        note = " (near a non-UTF-8 replacement char)" if self.replacement_nearby else ""
        return (
            f"{rel}:{self.line}: {self.tier} hit at cmd=0x{self.cmd:02X} "
            f"({protolib.cmd_name(self.cmd)}) declared_len={self.declared_len} "
            f"candidate_len={self.candidate_len} bytes={self.raw_prefix.hex().upper()}...{note}"
        )


def _line_number(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def _hex_runs(text: str) -> "list[tuple[bytes, list[int]]]":
    """Extract (decoded_bytes, per_byte_text_offsets) for every hex-shaped run of at least
    FRAME_MIN_SIZE decoded bytes -- below that no watched opcode can even appear at its fixed
    offset. Per-byte offsets are recorded during extraction (not re-derived afterwards) because
    the separator-tolerant regex makes reverse-locating bytes in the text ambiguous.
    """
    runs = []
    for run_match in _HEX_RUN_RE.finditer(text):
        run_start = run_match.start()
        run_text = run_match.group(0)
        byte_vals = []
        offsets = []
        for tok in _HEX_TOKEN_RE.finditer(run_text):
            hex_pair = tok.group(0)[-2:]  # strip an optional 0x/\x prefix
            byte_vals.append(int(hex_pair, 16))
            offsets.append(run_start + tok.start())
        if len(byte_vals) < protolib.FRAME_MIN_SIZE:
            continue
        runs.append((bytes(byte_vals), offsets))
    return runs


def _base64_runs(text: str) -> "list[tuple[bytes, list[int]]]":
    """Extract (decoded_bytes, per_byte_text_offsets) for every base64-shaped run that decodes to
    at least FRAME_MIN_SIZE bytes. Offsets are approximate (the run's start, repeated) -- base64
    is a speculative tier with no test asserting per-byte offset precision.
    """
    runs = []
    for run_match in _BASE64_RUN_RE.finditer(text):
        run_start = run_match.start()
        token = run_match.group(0)
        padded = token + "=" * (-len(token) % 4)
        try:
            decoded = base64.b64decode(padded, validate=False)
        except Exception:
            continue
        if len(decoded) < protolib.FRAME_MIN_SIZE:
            continue
        runs.append((decoded, [run_start] * len(decoded)))
    return runs


def _crc_le(buf: bytes, offset: int) -> int:
    return buf[offset] | (buf[offset + 1] << 8)


def _scan_buffer(buf: bytes, offsets: "list[int]", path: Path, text: str, is_hex_source: bool) -> "list[Finding]":
    """Port of radio_soft_phy.cpp::find_crc_valid_frame()'s algorithm: try every start offset,
    validate CTRL0 shape and CRC. Diverges from the C++ original in one deliberate way: candidate
    lengths are derived from CTRL0 (exactly declared_len, or declared_len + HMAC_SIZE only for
    CMD_ONEWAY_ADD_CONTROLLER's optional MAC trailer) rather than swept downward from a max bound
    -- the C++ sweep exists because its caller doesn't yet trust buffer alignment and leans on
    parse() to reject bad shapes; here the shape test itself is the check, so there is nothing to
    gain from retrying lengths CTRL0 already excludes.
    """
    findings = []
    buf_len = len(buf)
    for start in range(buf_len):
        if start + protolib.FRAME_MIN_SIZE > buf_len:
            break
        cmd = buf[start + FRAME_CMD_OFFSET]
        if cmd not in WATCHED_OPCODES:
            continue
        ctrl0 = buf[start]
        declared_len = protolib.ctrl0_implied_length(ctrl0)
        if not (protolib.FRAME_MIN_SIZE <= declared_len <= protolib.FRAME_MAX_SIZE):
            continue

        candidate_lens = [declared_len]
        if cmd == protolib.CMD_ONEWAY_ADD_CONTROLLER:
            candidate_lens.append(declared_len + protolib.HMAC_SIZE)

        for candidate_len in candidate_lens:
            if start + candidate_len > buf_len:
                continue

            crc_valid = False
            crc_end = start + candidate_len + FRAME_CRC_SIZE
            if crc_end <= buf_len:
                received_crc = _crc_le(buf, start + candidate_len)
                computed_crc = protolib.crc_ccitt(buf[start:start + candidate_len])
                crc_valid = received_crc == computed_crc

            if crc_valid:
                findings.append(_make_finding(buf, offsets, path, text, start, "crc-valid", cmd, declared_len,
                                              candidate_len))
                continue

            if not is_hex_source:
                continue
            # Tier 2 (CRC-less key-wrap shape): the whole run must be exactly one frame -- a
            # pasted log line, not byte soup -- and only the two key-carrying opcodes qualify.
            if start != 0 or start + candidate_len != buf_len:
                continue
            is_key_transfer_shape = cmd == protolib.CMD_KEY_TRANSFER and declared_len == KEY_TRANSFER_DECLARED_LEN
            is_add_controller_shape = (
                cmd == protolib.CMD_ONEWAY_ADD_CONTROLLER and declared_len == ONEWAY_ADD_CONTROLLER_DECLARED_LEN
            )
            if is_key_transfer_shape or is_add_controller_shape:
                findings.append(_make_finding(buf, offsets, path, text, start, "shape", cmd, declared_len,
                                              candidate_len))
    return findings


def _make_finding(buf: bytes, offsets: "list[int]", path: Path, text: str, start: int, tier: str, cmd: int,
                   declared_len: int, candidate_len: int) -> Finding:
    text_offset = offsets[start]
    window_start = max(0, text_offset - REPLACEMENT_WINDOW)
    window_end = min(len(text), text_offset + REPLACEMENT_WINDOW)
    replacement_nearby = REPLACEMENT_CHAR in text[window_start:window_end]
    raw_prefix = buf[start:start + min(16, candidate_len)]
    return Finding(path=path, line=_line_number(text, text_offset), tier=tier, cmd=cmd, declared_len=declared_len,
                   candidate_len=candidate_len, raw_prefix=raw_prefix, replacement_nearby=replacement_nearby)


def scan_text(path: Path, text: str) -> "list[Finding]":
    """Scan one file's already-decoded text for key-material findings. Exposed as a standalone
    function (path + text, not a filesystem read) so the self-test can exercise it against
    in-memory fixtures without writing files a future run of this very scanner would then have to
    tolerate.
    """
    findings = []
    for buf, offsets in _hex_runs(text):
        findings.extend(_scan_buffer(buf, offsets, path, text, is_hex_source=True))
    for buf, offsets in _base64_runs(text):
        findings.extend(_scan_buffer(buf, offsets, path, text, is_hex_source=False))
    return findings


def _is_corpus_captures_path(rel_path: str) -> bool:
    """Exact path-prefix exclusion for tests/corpus/captures/ -- that directory has its own,
    stronger crypto-verifying gate (scripts/corpus/validate.py). Exact-prefix, not a loose
    "contains 'corpus'" match: a path like docs/corpus-notes.yaml must NOT be excluded by this.
    """
    return rel_path.startswith(CORPUS_CAPTURES_PREFIX)


def in_scope_files() -> "list[str]":
    """Tracked *.md/*.txt/*.yaml/*.yml/*.cpp/*.h/*.hpp paths (repo-relative, forward-slashed),
    excluding tests/corpus/captures/. `git ls-files` structurally never sees reference/ or
    analysis/ (both git-excluded), so no code-level exclusion for either is needed here.
    """
    result = subprocess.run(["git", "ls-files", "--", *SCAN_PATTERNS], cwd=REPO_ROOT, check=True,
                            capture_output=True, text=True)
    files = [line for line in result.stdout.splitlines() if line and not _is_corpus_captures_path(line)]
    return sorted(files)


@dataclass
class SecretLeakFinding:
    path: Path
    line: int
    field_name: str

    def describe(self) -> str:
        rel = self.path.relative_to(REPO_ROOT) if self.path.is_absolute() else self.path
        return (
            f"{rel}:{self.line}: local secret leaked -- config/secrets.yaml's "
            f"'{self.field_name}' value appears verbatim"
        )


def _load_local_secret_key_values() -> "dict[str, str]":
    """Reads config/secrets.yaml (gitignored, local-only -- absent in CI and on a fresh clone) and
    returns {field_name: value} for every entry whose field name contains "key" (case-insensitive
    -- e.g. hub_system_key, api_key). Scoped to *_key fields, not every secret in the file: short
    values elsewhere (e.g. a 3-byte example device ID) coincide with joke placeholders already
    used deliberately in this repo's own test fixtures (C0FFEE), so treating every secret as
    forbidden would false-positive on those. Returns {} if the file doesn't exist.
    """
    if not SECRETS_FILE.exists():
        return {}
    with SECRETS_FILE.open(encoding="utf-8") as f:
        data = yaml.safe_load(f) or {}
    return {name: value for name, value in data.items() if isinstance(value, str) and value and "key" in name.lower()}


def _find_secret_occurrences(path: Path, text: str, secret_values: "dict[str, str]") -> "list[SecretLeakFinding]":
    """Plain verbatim substring search for each secret value in one file's text. Exposed as a
    standalone (path, text, secret_values) function, mirroring scan_text(), so the self-test can
    exercise it against in-memory fixtures without needing a real config/secrets.yaml on disk.
    """
    findings = []
    for field_name, value in secret_values.items():
        idx = text.find(value)
        if idx == -1:
            continue
        findings.append(SecretLeakFinding(path=path, line=_line_number(text, idx), field_name=field_name))
    return findings


def _all_tracked_files() -> "list[str]":
    result = subprocess.run(["git", "ls-files"], cwd=REPO_ROOT, check=True, capture_output=True, text=True)
    return sorted(line for line in result.stdout.splitlines() if line)


def scan_for_local_secret_leaks(secret_values: "dict[str, str]") -> "list[SecretLeakFinding]":
    """Greps every tracked file (the full repo, not just in_scope_files()'s doc/source subset --
    a literal substring search has no file-type-driven false-positive risk, so there's no reason
    to narrow it) for a verbatim occurrence of any local secret key value. Short-circuits to a
    no-op scan when secret_values is empty (no config/secrets.yaml locally, e.g. in CI or on a
    fresh clone) rather than walking the whole repo for nothing.
    """
    if not secret_values:
        return []
    findings = []
    for rel_path in _all_tracked_files():
        abs_path = REPO_ROOT / rel_path
        try:
            text = abs_path.read_text(encoding="utf-8", errors="strict")
        except (UnicodeDecodeError, OSError):
            continue  # binary/unreadable tracked file -- a literal secret string can't hide in it as text
        findings.extend(_find_secret_occurrences(Path(rel_path), text, secret_values))
    return findings


def main() -> int:
    paths = in_scope_files()
    all_findings: "list[Finding]" = []
    for rel_path in paths:
        abs_path = REPO_ROOT / rel_path
        text = abs_path.read_text(encoding="utf-8", errors="replace")
        all_findings.extend(scan_text(Path(rel_path), text))

    secret_values = _load_local_secret_key_values()
    secret_findings = scan_for_local_secret_leaks(secret_values)

    if all_findings or secret_findings:
        print("key-material-scan: FAILED", file=sys.stderr)
        for finding in all_findings:
            print(f"  {finding.describe()}", file=sys.stderr)
        for finding in secret_findings:
            print(f"  {finding.describe()}", file=sys.stderr)
        if all_findings:
            print(
                f"\n{len(all_findings)} frame finding(s) across {len(paths)} scanned file(s). "
                "Replace the offending bytes with prose, a note: reference to the corpus fixture "
                "that already holds them, or a deliberately-broken example (wrong CRC byte).",
                file=sys.stderr,
            )
        if secret_findings:
            print(
                f"\n{len(secret_findings)} local-secret leak finding(s). A value from your "
                "config/secrets.yaml appears verbatim in a tracked file -- remove it and, since it "
                "was committed to your working tree, rotate the underlying secret.",
                file=sys.stderr,
            )
        return 1

    secret_note = (
        f", {len(secret_values)} local secret key(s) checked" if secret_values
        else " -- no config/secrets.yaml found locally, secret-leak check skipped"
    )
    print(f"key-material-scan: OK ({len(paths)} file(s) scanned, 0 findings{secret_note})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
