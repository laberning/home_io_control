#!/usr/bin/env python3
"""Turns a pasted on-air log excerpt into a scaffolded golden-frame capture YAML.

Parses the two on-air log formats used by this project — the structured `io_capture` tag
(hub_internal.h) and the legacy `io_frame` tag (log_frame.h) — via protolib.parse_log(), with a
liberal fallback tier for mangled pastes. Emits a capture YAML skeleton (source metadata, raw
frames, and mechanically-derived `expect:` proposals) that a human must review and confirm
before committing — see tests/corpus/README.md :: "Expectations are human-verified". Every
capture this tool emits is `key: unknown` unless `--rekey` is given.

Usage:
  python3 scripts/corpus/ingest.py analysis/issues/27.txt \\
      --id issue_27_somfy_sunea_discovery --device "Somfy Sunea IO motor" \\
      --captured-with sx1262 --origin github-issue \\
      --issue https://github.com/laberning/home_io_control/issues/27 --date 2026-07-06 \\
      -o tests/corpus/captures/issues/issue_27_somfy_sunea_discovery.yaml

Read from stdin instead of a file with `-` as the input path — handy for piping a trimmed
excerpt (`sed -n '10,40p' analysis/issues/27.txt | python3 scripts/corpus/ingest.py - ...`)
instead of hand-editing a scratch file.

Re-key mode (--rekey) — run ONLY locally, by someone who has the real system
key, and ONLY on your own hardware captures:
  python3 scripts/corpus/ingest.py my_pairing_capture.log --rekey \\
      --system-key-from config/secrets.yaml \\
      --id somfy_awning_exchange_open --device "..." --captured-with sx1262 \\
      --origin own-hardware --date 2026-07-06 \\
      -o tests/corpus/captures/somfy_awning/exchange_open.yaml
This verifies every captured HMAC / key-transfer payload against the real key (hard abort on
any mismatch), rewrites them under the public corpus key, and emits `key: corpus`.
`--system-key-from` refuses to read a path that is not git-ignored. Only the system key is
secret (see tests/corpus/README.md :: "Key hygiene") — node IDs are kept as captured by
default; `--remap OLDHEX=NEWHEX`/`--role NEWHEX=name` exist if you want to anonymize a specific
node ID for some other reason, but are optional, not required for privacy.

A capture containing a CMD_ONEWAY_ADD_CONTROLLER (0x30) / CMD_ONEWAY_REMOVE (0x39) frame also
needs `--oneway-key-from <path>` — a 1W controller's key is frequently NOT the same as the hub's
own `hub_system_key`, so it is resolved and verified independently. Point it at wherever you
pasted the "Recover 1W Controller Key" switch's report —
it emits the recovered key inline in a ready-to-paste `oneway_controllers:` block, so any
git-ignored file containing that block works, `config/secrets.yaml` or otherwise:
  python3 scripts/corpus/ingest.py my_1w_capture.log --rekey \\
      --oneway-key-from config/secrets.yaml \\
      --id somfy_smoove_9d6085_add_and_remove_controller --device "Somfy Smoove io 1W remote" \\
      --captured-with sx1276 --origin own-hardware --date 2026-08-16 \\
      -o tests/corpus/captures/somfy_smoove_9d6085/oneway_add_and_remove_controller.yaml
`--system-key-from` and `--oneway-key-from` are each only required when a capture actually needs
them — a pure-1W capture with no 0x3C/0x3D/0x32/0x2A frames needs no `--system-key-from`, and vice
versa.
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import protolib  # noqa: E402
from protolib import cmd_name, ctrl0_implied_length, is_end, is_oneway, is_start, parse_log  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent.parent


class RekeyError(Exception):
    """A --rekey verification or safety check failed; the pipeline must abort, not degrade."""


def read_input(path: str) -> str:
    if path == "-":
        return sys.stdin.read()
    return Path(path).read_text(encoding="utf-8", errors="replace")


def normalize_t_ms(frames) -> None:
    """Rebase every frame's t_ms to be relative to the earliest known timestamp in the set."""
    known = [f.t_ms for f in frames if f.t_ms is not None]
    if not known:
        return
    base = min(known)
    for frame in frames:
        if frame.t_ms is not None:
            frame.t_ms -= base


def yaml_quote(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def render_frame_yaml(frame) -> str:
    lines = [f"  - dir: {frame.direction}"]
    if frame.t_ms is not None:
        lines.append(f"    t_ms: {frame.t_ms}")
    if frame.freq:
        lines.append(f"    freq: {frame.freq}")
    lines.append(f"    hex: \"{frame.hex_bytes.upper()}\"")
    lines.append(f"    crc: {'present' if frame.crc_present() else 'absent'}")

    note_bits = []
    if frame.unverified:
        note_bits.append("UNVERIFIED-EXTRACTION: could not confirm frame shape from a known log tag")
    raw = frame.raw()
    if not frame.unverified and len(raw) >= FRAME_MIN_SIZE_FOR_NOTE:
        ctrl0 = raw[0]
        if len(raw) == ctrl0_implied_length(ctrl0):
            cmd = raw[8]
            note_bits.append(f"cmd={cmd_name(cmd)}(0x{cmd:02X})")
    if note_bits:
        lines.append(f"    note: {yaml_quote('; '.join(note_bits))}")
    return "\n".join(lines)


FRAME_MIN_SIZE_FOR_NOTE = 9  # mirrors protolib.FRAME_MIN_SIZE; raw[8] (cmd byte) needs len>=9


def render_expect_frame_yaml(frame) -> "str | None":
    """Mechanically-derivable proposal from CTRL0 bits alone: cmd, start/end, 1w/2w. Never
    proposes decoded semantics (1W intent, device name, position) — those need the real decoder
    and, per the corpus's human-verification rule, must be confirmed against the issue thread's
    established facts rather than rubber-stamped from any decoder's current output.
    """
    raw = frame.raw()
    if frame.unverified or len(raw) < FRAME_MIN_SIZE_FOR_NOTE:
        return None
    ctrl0 = raw[0]
    if len(raw) != ctrl0_implied_length(ctrl0):
        return None  # length disagrees with CTRL0 — extraction is unreliable, propose nothing
    cmd = raw[8]
    protocol = "1w" if is_oneway(ctrl0) else "2w"
    return (f"    - {{cmd: 0x{cmd:02X}, start: {str(is_start(ctrl0)).lower()}, "
            f"end: {str(is_end(ctrl0)).lower()}, protocol: {protocol}}}  # PROPOSED — verify before commit")


def build_yaml(args, frames, key_mode: str, node_map: "dict[str, str] | None") -> str:
    unverified_count = sum(1 for f in frames if f.unverified)
    description = args.description or f"Scaffolded from {args.input} by ingest.py — TODO: describe the scenario."

    lines = [
        f"id: {args.id}",
        "description: >",
        f"  {description}",
        "source:",
        f"  device: {yaml_quote(args.device)}",
        f"  captured_with: {args.captured_with}",
        f"  firmware: {yaml_quote(args.firmware) if args.firmware else 'null'}",
        f"  date: {args.date}",
        f"  origin: {args.origin}",
        f"  issue: {yaml_quote(args.issue) if args.issue else 'null'}",
        f"key: {key_mode}",
    ]
    if node_map:
        lines.append("node_map:")
        lines.extend(f"  {role}: {yaml_quote(node_id)}" for node_id, role in node_map.items())
    lines.append("frames:")
    lines.extend(render_frame_yaml(frame) for frame in frames)

    expect_lines = [line for line in (render_expect_frame_yaml(f) for f in frames) if line is not None]
    if expect_lines:
        lines.append("expect:")
        lines.append("  frames:")
        lines.extend(expect_lines)

    if unverified_count:
        print(f"ingest.py: {unverified_count} frame(s) marked UNVERIFIED-EXTRACTION — review before committing",
              file=sys.stderr)

    return "\n".join(lines) + "\n"


# ==================================================================================================
# --rekey pipeline (design doc §4.2)
# ==================================================================================================


def key_fingerprint(key: bytes) -> str:
    """Short, non-reversible fingerprint for logging — the real key itself must never be printed."""
    return hashlib.sha256(key).hexdigest()[:8]


def resolve_system_key(args) -> bytes:
    """Resolve the real system key from --system-key-from <path> or IOHOME_SYSTEM_KEY. Refuses
    to read a path that is not git-ignored — the whole point of --rekey is that the real key
    never ends up in a committable file.
    """
    if args.system_key_from is None:
        env_value = os.environ.get("IOHOME_SYSTEM_KEY")
        if env_value is None:
            raise RekeyError("--rekey requires --system-key-from <path> or the IOHOME_SYSTEM_KEY env var")
        try:
            key = bytes.fromhex(env_value.strip().replace(" ", ""))
        except ValueError as exc:
            raise RekeyError(f"IOHOME_SYSTEM_KEY is not valid hex ({exc})") from exc
    else:
        path = Path(args.system_key_from)
        result = subprocess.run(["git", "check-ignore", str(path)], cwd=REPO_ROOT, capture_output=True, text=True,
                                check=False)
        if result.returncode != 0:
            raise RekeyError(f"--system-key-from refuses to read {path}: not git-ignored "
                             f"(run `git check-ignore {path}` to confirm before pointing --rekey at it)")
        text = path.read_text(encoding="utf-8")
        match = re.search(r"(?:hub_system_key|system_key)[^0-9A-Fa-f]*([0-9A-Fa-f]{32})", text)
        if match is None:
            match = re.search(r"\b([0-9A-Fa-f]{32})\b", text)
        if match is None:
            raise RekeyError(f"could not find a 32-hex-digit system key in {path}")
        key = bytes.fromhex(match.group(1))
    if len(key) != protolib.AES_KEY_SIZE:
        raise RekeyError(f"resolved system key is {len(key)} bytes, expected {protolib.AES_KEY_SIZE}")
    return key


def resolve_oneway_key(args) -> bytes:
    """Resolve the real 1W controller key from --oneway-key-from <path> or IOHOME_ONEWAY_KEY.

    Deliberately separate from resolve_system_key(), not a fallback to it: a 1W controller's key
    is frequently NOT the hub's own 2W hub_system_key -- a real Smoove remote's adopted 1W key has
    been observed to differ from its own hub's hub_system_key. Silently reusing the wrong key here
    would make rekey_oneway_*_frames() below either hard-abort (if the frames happen not to
    verify) or, worse, succeed against the wrong key by coincidence. Same git-ignore refusal as
    resolve_system_key().
    """
    if args.oneway_key_from is None:
        env_value = os.environ.get("IOHOME_ONEWAY_KEY")
        if env_value is None:
            raise RekeyError(
                "this capture contains a CMD_ONEWAY_ADD_CONTROLLER/CMD_ONEWAY_REMOVE frame, which needs the "
                "real 1W controller key -- pass --oneway-key-from <path> or set IOHOME_ONEWAY_KEY "
                "(this is frequently NOT the same value as hub_system_key)")
        try:
            key = bytes.fromhex(env_value.strip().replace(" ", ""))
        except ValueError as exc:
            raise RekeyError(f"IOHOME_ONEWAY_KEY is not valid hex ({exc})") from exc
    else:
        path = Path(args.oneway_key_from)
        result = subprocess.run(["git", "check-ignore", str(path)], cwd=REPO_ROOT, capture_output=True, text=True,
                                check=False)
        if result.returncode != 0:
            raise RekeyError(f"--oneway-key-from refuses to read {path}: not git-ignored "
                             f"(run `git check-ignore {path}` to confirm before pointing --rekey at it)")
        text = path.read_text(encoding="utf-8")
        # No fixed field name to key off: the hub's own adoption report (hub_internal.h ::
        # build_oneway_adoption_report()) emits the recovered key inline in the pasteable YAML
        # block (`system_key: "..."`) rather than naming a secrets.yaml field, mirroring the 2W
        # key-extraction report's style. Any 32-hex-digit run in the pointed-at file is accepted,
        # same fallback resolve_system_key() uses.
        match = re.search(r"\b([0-9A-Fa-f]{32})\b", text)
        if match is None:
            raise RekeyError(f"could not find a 32-hex-digit 1W controller key in {path}")
        key = bytes.fromhex(match.group(1))
    if len(key) != protolib.AES_KEY_SIZE:
        raise RekeyError(f"resolved 1W controller key is {len(key)} bytes, expected {protolib.AES_KEY_SIZE}")
    return key


def parse_remap_args(remap_args: "list[str]", role_args: "list[str]") -> "tuple[dict, dict]":
    """--remap OLD=NEW (repeatable) -> {old_bytes: new_bytes}; --role NEW=name -> {new_hex: role}."""
    remap = {}
    for entry in remap_args or []:
        old_hex, _, new_hex = entry.partition("=")
        try:
            old_bytes = bytes.fromhex(old_hex.strip())
            new_bytes = bytes.fromhex(new_hex.strip())
        except ValueError as exc:
            raise RekeyError(f"--remap {entry!r} is not valid hex ({exc})") from exc
        if len(old_bytes) != 3 or len(new_bytes) != 3:
            raise RekeyError(f"--remap {entry!r}: node IDs must be 3 bytes (6 hex digits) each")
        remap[old_bytes] = new_bytes
    roles = {}
    for entry in role_args or []:
        new_hex, _, role = entry.partition("=")
        roles[new_hex.strip().upper()] = role
    return remap, roles


def apply_node_remap(frames, remap: "dict[bytes, bytes]") -> "list[str]":
    """Replace 3-byte node IDs in every frame's SRC/DST header fields. Payload bytes are only
    scanned and flagged (never blind-replaced) — some payloads (e.g. discovery responses) embed
    the node ID again in the data, and a false-positive 3-byte match in unrelated payload content
    is possible, so this needs a human's eyes rather than an automatic rewrite.
    """
    if not remap:
        return []
    warnings = []
    for frame in frames:
        raw = bytearray(frame.raw())
        if len(raw) < protolib.FRAME_MIN_SIZE:
            continue
        for old_id, new_id in remap.items():
            if bytes(raw[2:5]) == old_id:
                raw[2:5] = new_id
            if bytes(raw[5:8]) == old_id:
                raw[5:8] = new_id
            # Flag (not replace) payload occurrences of the old ID.
            payload = bytes(raw[protolib.FRAME_MIN_SIZE:])
            if old_id in payload:
                warnings.append(f"frame src/dst-remapped but payload still contains old node ID "
                                f"{old_id.hex().upper()} — review manually (e.g. discovery response "
                                f"payloads that repeat the node ID): {bytes(raw).hex().upper()}")
        _set_frame_raw(frame, bytes(raw))
    return warnings


def _set_frame_raw(frame, new_non_crc_bytes: bytes) -> None:
    """Overwrite a RawFrame's hex content, recomputing the trailing CRC if it had one.

    Must be called with `frame` still holding its *original* (pre-overwrite) bytes — CRC
    presence is re-derived from those via frame.crc_present() before frame.hex_bytes is
    replaced. Callers build `new_non_crc_bytes` from protolib.non_crc_bytes(frame) (also read
    before this call), so the two stay in sync by construction.
    """
    if frame.crc_present():
        crc = protolib.crc_ccitt(new_non_crc_bytes)
        new_raw = new_non_crc_bytes + bytes([crc & 0xFF, (crc >> 8) & 0xFF])
    else:
        new_raw = new_non_crc_bytes
    frame.hex_bytes = " ".join(f"{b:02X}" for b in new_raw)


def rekey_hmac_frames(frames, real_key: bytes, corpus_key: bytes) -> int:
    """Verify + rewrite every 0x3C/0x3D authenticated-exchange HMAC. Hard aborts (does not
    silently skip) on the first frame whose captured HMAC does not verify under the real key —
    a mismatch means either the wrong key was supplied or the capture is corrupted/mis-copied,
    and either way the corpus must not silently accept it.
    """
    rewritten = 0
    for origin, challenge, response in protolib.find_challenge_response_triples(frames):
        parts = protolib.hmac_parts(origin, challenge, response)

        if not protolib.verify_hmac(parts.transcript, parts.hmac, parts.challenge, real_key):
            raise RekeyError(
                f"HMAC verification FAILED against the real key (fingerprint sha256={key_fingerprint(real_key)}) "
                f"for challenge-response frame {response.raw().hex().upper()} — wrong key, mis-copied bytes, or a "
                "corrupted capture. Aborting; nothing was written.")

        new_hmac = protolib.create_hmac(parts.transcript, parts.challenge, corpus_key)
        new_response = protolib.non_crc_bytes(response)[:protolib.FRAME_MIN_SIZE] + new_hmac
        _set_frame_raw(response, new_response)
        rewritten += 1
    return rewritten


def rekey_key_transfer_frames(frames, real_key: bytes, corpus_key: bytes) -> int:
    """Verify + rewrite every 0x32 key-transfer payload. This is the security-critical path
    (design §4.3): a raw, un-re-keyed 0x32 payload leaks the real system key to anyone who can
    decrypt it with the public TRANSFER_KEY, so this function either produces a payload that
    decrypts to the *corpus* key, or aborts before writing anything.
    """
    rewritten = 0
    for key_init, challenge, transfer in protolib.find_key_transfer_triples(frames):
        parts = protolib.key_transfer_parts(key_init, challenge, transfer)

        decrypted = protolib.crypt_key(parts.key_init_cmd, parts.challenge, parts.encrypted_payload)
        if decrypted != real_key:
            raise RekeyError(
                f"key-transfer payload does NOT decrypt to the real key (fingerprint sha256="
                f"{key_fingerprint(real_key)}) for frame {transfer.raw().hex().upper()} — aborting; "
                "nothing was written. Never commit this capture un-re-keyed.")

        new_encrypted = protolib.crypt_key(parts.key_init_cmd, parts.challenge, corpus_key)
        new_transfer = protolib.non_crc_bytes(transfer)[:protolib.FRAME_MIN_SIZE] + new_encrypted
        _set_frame_raw(transfer, new_transfer)
        rewritten += 1
    return rewritten


def rekey_oneway_add_controller_frames(frames, real_oneway_key: bytes, corpus_key: bytes) -> int:
    """Verify + rewrite every CMD_ONEWAY_ADD_CONTROLLER (0x30) frame's key-wrap, and its
    out-of-length MAC trailer when present. Same tier as rekey_key_transfer_frames() (design
    §4.3): `enc_key` is wrapped only under the public TRANSFER_KEY with an IV derived from the
    sender's own node address, itself plaintext in the frame header -- so an un-re-keyed payload
    leaks the real 1W controller key to anyone who reads it. Hard aborts rather than silently
    skipping, same contract as every other rewriter here.

    The trailer is optional (protolib.oneway_add_controller_parts()'s docstring): real Somfy
    hardware omits it even though the published documentation vector carries one, so this only
    verifies+rewrites a MAC when the captured frame actually has one.
    """
    rewritten = 0
    for frame in protolib.find_oneway_add_controller_frames(frames):
        parts = protolib.oneway_add_controller_parts(frame)
        unwrapped = protolib.crypt_1w_key(parts.src, parts.enc_key)
        if unwrapped != real_oneway_key:
            raise RekeyError(
                f"0x30 enc_key does NOT unwrap to the real 1W key (fingerprint sha256="
                f"{key_fingerprint(real_oneway_key)}) for frame {frame.raw().hex().upper()} — wrong key, "
                "mis-copied bytes, or a corrupted capture. Aborting; nothing was written.")

        new_enc_key = protolib.crypt_1w_key(parts.src, corpus_key)
        new_payload = new_enc_key + parts.rest

        if parts.trailer:
            span = bytes([protolib.CMD_ONEWAY_ADD_CONTROLLER]) + parts.enc_key
            expected_mac = protolib.create_1w_hmac(span, parts.sequence, real_oneway_key)
            if expected_mac != parts.trailer:
                raise RekeyError(f"0x30 MAC trailer does NOT verify under the real 1W key for frame "
                                 f"{frame.raw().hex().upper()} — aborting; nothing was written.")
            new_span = bytes([protolib.CMD_ONEWAY_ADD_CONTROLLER]) + new_enc_key
            new_payload += protolib.create_1w_hmac(new_span, parts.sequence, corpus_key)

        new_raw = protolib.non_crc_bytes(frame)[:protolib.FRAME_MIN_SIZE] + new_payload
        _set_frame_raw(frame, new_raw)
        rewritten += 1
    return rewritten


def rekey_oneway_remove_controller_frames(frames, real_oneway_key: bytes, corpus_key: bytes) -> int:
    """Verify + rewrite every CMD_ONEWAY_REMOVE (0x39) frame's MAC.

    Unlike 0x30's enc_key, this MAC does not directly encode key material — it is a truncated
    block-cipher output, not recoverable back to the key — but it is rewritten under the corpus
    key anyway, for the same reason every other captured HMAC in this pipeline is: `key: corpus`
    promises every signature in the capture verifies under the public key, not the real one.
    Span is `cmd + data` (protolib.create_1w_hmac()'s doxygen) — confirmed against 11 real
    CMD_ONEWAY_REMOVE captures from the same real-hardware session this function was built to
    re-key, not merely assumed.
    """
    rewritten = 0
    for frame in protolib.find_oneway_remove_controller_frames(frames):
        parts = protolib.oneway_remove_controller_parts(frame)
        expected_mac = protolib.create_1w_hmac(parts.span, parts.sequence, real_oneway_key)
        if expected_mac != parts.mac:
            raise RekeyError(f"0x39 MAC does NOT verify under the real 1W key for frame "
                             f"{frame.raw().hex().upper()} — aborting; nothing was written.")
        new_mac = protolib.create_1w_hmac(parts.span, parts.sequence, corpus_key)
        sequence_bytes = bytes([(parts.sequence >> 8) & 0xFF, parts.sequence & 0xFF])
        new_payload = parts.data + sequence_bytes + new_mac
        new_raw = protolib.non_crc_bytes(frame)[:protolib.FRAME_MIN_SIZE] + new_payload
        _set_frame_raw(frame, new_raw)
        rewritten += 1
    return rewritten


def rekey_self_authenticated_frames(frames, real_key: bytes, corpus_key: bytes) -> int:
    """Verify + rewrite the HMAC half of every self-authenticated frame (payload =
    [challenge | HMAC], see protolib.SELF_AUTHENTICATED_COMMANDS). The challenge half is left
    exactly as captured — it is random bytes the sender chose, not key-derived, and rewriting it
    would destroy a real recorded value for no gain.

    Same hard-abort contract as the other two rewriters: a payload of the right length whose HMAC
    does not verify means the assumed shape is wrong for this frame (or the bytes are corrupt),
    and guessing would silently replace real capture data with fiction.
    """
    rewritten = 0
    for frame in protolib.find_self_authenticated_frames(frames):
        transcript, challenge, hmac = protolib.self_auth_parts(frame)
        if not protolib.verify_hmac(transcript, hmac, challenge, real_key):
            raise RekeyError(
                f"self-authenticated frame {frame.raw().hex().upper()} has a "
                f"[challenge|HMAC]-shaped payload that does NOT verify under the real key "
                f"(fingerprint sha256={key_fingerprint(real_key)}) — wrong key, corrupted capture, or a "
                "command wrongly listed in protolib.SELF_AUTHENTICATED_COMMANDS. Aborting; nothing was written.")
        new_hmac = protolib.create_hmac(transcript, challenge, corpus_key)
        prefix = protolib.non_crc_bytes(frame)[:protolib.FRAME_MIN_SIZE]
        _set_frame_raw(frame, prefix + challenge + new_hmac)
        rewritten += 1
    return rewritten


def assert_no_key_leakage(frames, real_key: bytes) -> None:
    """Output-safety scan: after rewriting, no frame may contain the real key's bytes in either
    byte order. This is a last-resort net, not the primary defense (rekey_*_frames already
    verify+rewrite every crypto-bearing frame) — it exists to catch a rekey bug, not to be relied
    on as the only safeguard.
    """
    reversed_key = bytes(reversed(real_key))
    for frame in frames:
        raw = frame.raw()
        if real_key in raw or reversed_key in raw:
            raise RekeyError(
                f"OUTPUT-SAFETY ABORT: a rewritten frame still contains the real key's bytes "
                f"(fingerprint sha256={key_fingerprint(real_key)}) — refusing to write output. "
                f"frame={raw.hex().upper()}")


def apply_rekey(frames, args) -> "tuple[bytes, list[str], dict[str, str]]":
    """Run the full --rekey pipeline in place on `frames`. Returns (real_key, warnings,
    node_map) — the caller uses the returned key only for log messages (fingerprint only, never
    the key itself) since assert_no_key_leakage() already ran before returning. `real_key` is the
    2W system key (None if this capture has no 2W crypto at all — a pure 1W capture needs no
    --system-key-from).

    The 2W system key and the 1W controller key are resolved and required independently, each
    only when frames of that kind are actually present: a --system-key-from that isn't needed
    (pure 1W capture) or a --oneway-key-from that isn't needed (pure 2W capture) must not become a
    spurious requirement. The two keys can differ (see resolve_oneway_key()'s docstring), so
    neither is ever used to satisfy the other.
    """
    corpus_key = protolib.CORPUS_SYSTEM_KEY
    remap, roles = parse_remap_args(args.remap, args.role)

    has_2w_crypto = bool(protolib.find_key_transfer_triples(frames) or
                         protolib.find_challenge_response_triples(frames) or
                         protolib.find_self_authenticated_frames(frames))
    has_oneway = bool(protolib.find_oneway_add_controller_frames(frames) or
                      protolib.find_oneway_remove_controller_frames(frames))

    real_key = resolve_system_key(args) if has_2w_crypto else None
    real_oneway_key = resolve_oneway_key(args) if has_oneway else None

    summary_parts = []
    if has_2w_crypto:
        hmac_count = rekey_hmac_frames(frames, real_key, corpus_key)
        hmac_count += rekey_self_authenticated_frames(frames, real_key, corpus_key)
        transfer_count = rekey_key_transfer_frames(frames, real_key, corpus_key)
        summary_parts.append(f"{hmac_count} HMAC frame(s), {transfer_count} key-transfer frame(s) under real "
                             f"system key (fingerprint sha256={key_fingerprint(real_key)})")
    if has_oneway:
        add_count = rekey_oneway_add_controller_frames(frames, real_oneway_key, corpus_key)
        remove_count = rekey_oneway_remove_controller_frames(frames, real_oneway_key, corpus_key)
        summary_parts.append(f"{add_count} 1W add-controller frame(s), {remove_count} 1W remove-controller "
                             f"frame(s) under real 1W key (fingerprint sha256="
                             f"{key_fingerprint(real_oneway_key)})")

    warnings = apply_node_remap(frames, remap)
    if real_key is not None:
        assert_no_key_leakage(frames, real_key)
    if real_oneway_key is not None:
        assert_no_key_leakage(frames, real_oneway_key)

    print(f"ingest.py --rekey: verified+rewrote " + "; ".join(summary_parts) +
          f" -> corpus key (fingerprint sha256={key_fingerprint(corpus_key)})")
    for warning in warnings:
        print(f"ingest.py --rekey: WARNING: {warning}", file=sys.stderr)

    node_map = {new_id.hex().upper(): roles.get(new_id.hex().upper(), new_id.hex().upper())
               for new_id in remap.values()} if remap else {}
    return real_key, warnings, node_map


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", help="log file to parse, or '-' for stdin")
    parser.add_argument("--id", required=True, help="capture id (globally unique across the corpus)")
    parser.add_argument("--device", required=True, help="free-text device description")
    parser.add_argument("--captured-with", default="other",
                        choices=["sx1276", "sx1262", "lr1121", "other", "synthetic"])
    parser.add_argument(
        "--origin",
        required=True,
        choices=["own-hardware", "github-issue", "synthetic-bootstrap", "reference-material"],
    )
    parser.add_argument("--issue", default=None, help="issue URL, e.g. https://github.com/.../issues/27")
    parser.add_argument("--date", required=True, help="YYYY-MM-DD")
    parser.add_argument("--firmware", default=None)
    parser.add_argument("--description", default=None, help="free-text scenario summary")
    parser.add_argument("-o", "--output", required=True, help="output capture YAML path")
    parser.add_argument("--rekey", action="store_true",
                        help="verify captured crypto against the real key and rewrite it under the corpus key")
    parser.add_argument("--system-key-from", default=None,
                        help="path to a git-ignored file containing the real system key (32 hex digits)")
    parser.add_argument("--oneway-key-from", default=None,
                        help="path to a git-ignored file containing the real 1W controller key (32 hex digits) "
                             "-- only needed if the capture contains a CMD_ONEWAY_ADD_CONTROLLER/"
                             "CMD_ONEWAY_REMOVE frame; not necessarily the same key as --system-key-from")
    parser.add_argument("--remap", action="append", default=[], metavar="OLDHEX=NEWHEX",
                        help="remap a 3-byte node ID (repeatable)")
    parser.add_argument("--role", action="append", default=[], metavar="NEWHEX=name",
                        help="label a remapped node ID's role for node_map: (repeatable)")
    args = parser.parse_args()

    text = read_input(args.input)
    frames = parse_log(text)
    if not frames:
        print("ingest.py: no frames extracted from input", file=sys.stderr)
        return 1
    normalize_t_ms(frames)

    key_mode = "unknown"
    node_map = {}
    if args.rekey:
        try:
            _, _, node_map = apply_rekey(frames, args)
        except RekeyError as exc:
            print(f"ingest.py --rekey: ABORT: {exc}", file=sys.stderr)
            return 1
        key_mode = "corpus"

    rendered = build_yaml(args, frames, key_mode, node_map)
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(rendered, encoding="utf-8")
    unverified_count = sum(1 for f in frames if f.unverified)
    print(f"ingest.py: wrote {output_path} ({len(frames)} frames, {unverified_count} unverified)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
