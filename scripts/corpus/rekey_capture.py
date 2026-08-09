#!/usr/bin/env python3
"""Re-keys a pairing capture YAML using the key recovered from the capture's own 0x32 frame.

`ingest.py --rekey` re-keys a capture you already hold the real system key for (your own
hardware, `--system-key-from`). This tool covers the other case: a `reference-material` pairing
capture (ADR 0023) transcribed from someone else's published log, where the system key is a
third party's and exists nowhere but inside the capture. The `0x32` CMD_KEY_TRANSFER payload is
encrypted under TRANSFER_KEY — public and identical in every io-homecontrol installation — so
that key is trivially recoverable from the bytes alone, which is exactly why such a capture can
never be committed as transcribed.

So: recover it in memory, hand it straight to the same verify-and-rewrite pipeline
(`ingest.py :: apply_rekey`), write out a capture whose crypto is the public corpus key, and
never print or persist the recovered key. The real key exists only as a local variable in one
process; the only thing said about it out loud is a sha256 fingerprint, which identifies runs
without revealing bytes.

    python3 scripts/corpus/rekey_capture.py /path/outside/repo/raw_capture.yaml \\
        -o tests/corpus/captures/velux_kux100/pairing_full.yaml

`--system-key-from` covers a capture with no 0x32 to recover a key from — an own-hardware exchange
whose only crypto is a 0x3D HMAC, hand-assembled from a log rather than piped through `ingest.py`:

    python3 scripts/corpus/rekey_capture.py --system-key-from config/secrets.yaml \\
        ~/outside-the-repo/exchange.yaml -o tests/corpus/captures/somfy_awning/exchange.yaml

Either way this runs **before** the capture's first commit. A capture that reaches `main` still
holding real key material is deleted and re-recorded, never rewritten in place — see
tests/corpus/README.md :: "Raw bytes are immutable" for why, and its "Pending re-record" table for
anything currently in that state.

Everything except the rewritten `hex:` values and the `key:` mode is passed through verbatim —
this edits the file as text rather than re-emitting parsed YAML, so hand-written descriptions,
notes and `expect:` blocks survive a re-key untouched. Prose is *not* rewritten, so a note
saying "HMAC not verifiable" that was true while the capture was `key: unknown` is flagged for
review rather than silently left contradicting the bytes above it.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ingest  # noqa: E402
import protolib  # noqa: E402
import validate  # noqa: E402
from ingest import RekeyError, key_fingerprint  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent.parent

_HEX_LINE_RE = re.compile(r'^(?P<indent>\s*)hex:\s*"(?P<hex>[0-9A-Fa-f ]+)"\s*$')
_KEY_LINE_RE = re.compile(r"^key:\s*(?P<mode>\S+)\s*$")


def assert_input_is_not_committable(path: Path) -> None:
    """Refuse an input path that git could commit — until this tool runs, the file is a verbatim
    record of a real system key.

    A tracked path is refused too, and deliberately so: re-keying an already-committed capture is
    not a supported repair (tests/corpus/README.md :: "Raw bytes are immutable" — such a capture is
    deleted and re-recorded instead), so the tool must not offer a way to do it.
    """
    try:
        resolved = path.resolve().relative_to(REPO_ROOT)
    except ValueError:
        return  # outside the repo entirely — nothing git could commit
    ignored = subprocess.run(["git", "check-ignore", str(resolved)], cwd=REPO_ROOT, capture_output=True, text=True,
                             check=False)
    if ignored.returncode != 0:
        raise RekeyError(
            f"refusing to read {resolved}: it is inside the repo and not git-ignored, so it either already holds "
            "real key material in git history (delete and re-record it — see tests/corpus/README.md :: 'Raw bytes "
            "are immutable') or is one `git add` from doing so. Keep the pre-re-key capture outside the repo, or "
            "under a git-ignored path, and write the re-keyed result into the corpus with -o")


def load_frames(capture: dict) -> "list[protolib.RawFrame]":
    """Capture-YAML frames as RawFrames holding their full `hex` bytes (CRC included when
    present) — RawFrame.crc_present() then detects the trailing CRC on its own, so the rewrite
    helpers recompute it after changing a payload.
    """
    return [protolib.RawFrame(frame["dir"], frame["hex"]) for frame in capture["frames"]]


def resolve_key(frames: "list[protolib.RawFrame]", system_key_from: "str | None") -> "tuple[bytes, str]":
    """Get the real key either from an explicit git-ignored key file or from the capture's own
    0x32 frame. Returns (key, how) for the log line.

    The explicit path exists for captures with no 0x32 to recover from — most importantly an
    own-hardware capture that was committed without a re-key and still carries HMACs computed
    under the maintainer's real key. Reuses ingest.resolve_system_key(), so the "key file must be
    git-ignored" refusal applies identically here.
    """
    if system_key_from is not None:
        key = ingest.resolve_system_key(SimpleNamespace(system_key_from=system_key_from))
        if key == protolib.CORPUS_SYSTEM_KEY:
            raise RekeyError("the supplied key file contains the public corpus key, not a real system key")
        return key, f"supplied key file {system_key_from}"
    return recover_system_key(frames), "the capture's own 0x32 key-transfer frame"


def recover_system_key(frames: "list[protolib.RawFrame]") -> bytes:
    """Decrypt the capture's own 0x32 payload with the public TRANSFER_KEY to recover the system
    key it was captured under. Multiple key-transfer frames must all recover the same key — one
    capture is one pairing session, so a disagreement means the frames were mis-assembled (or
    two sessions were spliced together) and the re-key would silently verify against the wrong
    key for some of them.
    """
    triples = protolib.find_key_transfer_triples(frames)
    if not triples:
        raise RekeyError(
            "no 0x31/0x3C/0x32 key-transfer triple in this capture — there is no key to recover from it. "
            "A capture whose only crypto is HMACs must be re-keyed with `ingest.py --rekey --system-key-from` "
            "by someone who independently holds the key, or stay `key: unknown`")
    recovered = set()
    for key_init, challenge, transfer in triples:
        parts = protolib.key_transfer_parts(key_init, challenge, transfer)
        recovered.add(protolib.crypt_key(parts.key_init_cmd, parts.challenge, parts.encrypted_payload))
    if len(recovered) > 1:
        raise RekeyError(
            f"the capture's {len(triples)} key-transfer frames decrypt to {len(recovered)} different keys "
            "(fingerprints " + ", ".join(sorted(key_fingerprint(k) for k in recovered)) +
            ") — this is not one coherent pairing session; aborting")
    key = recovered.pop()
    if key == protolib.CORPUS_SYSTEM_KEY:
        raise RekeyError("this capture is already re-keyed (its 0x32 payload decrypts to the public corpus key) "
                         "— nothing to do")
    return key


def rewrite_text(text: str, frames: "list[protolib.RawFrame]", original_hex: "list[str]") -> "tuple[str, int]":
    """Substitute rewritten frame bytes back into the original file text, leaving every other
    byte of the file (description, notes, expect:) exactly as the human wrote it. The Nth
    `hex:` line in the file is the Nth entry of `frames:` — the schema has no other `hex:` key.
    """
    lines = text.splitlines(keepends=True)
    index = 0
    changed = 0
    for line_no, line in enumerate(lines):
        match = _HEX_LINE_RE.match(line.rstrip("\n"))
        if match is None:
            continue
        if index >= len(frames):
            raise RekeyError(f"more `hex:` lines in the file than frames parsed from it (line {line_no + 1})")
        if "".join(match.group("hex").split()).upper() != "".join(original_hex[index].split()).upper():
            raise RekeyError(f"line {line_no + 1} does not match frames[{index}] as parsed — refusing to rewrite")
        new_hex = frames[index].hex_bytes.upper()
        if new_hex != original_hex[index].upper():
            newline = "\n" if line.endswith("\n") else ""
            lines[line_no] = f'{match.group("indent")}hex: "{new_hex}"{newline}'
            changed += 1
        index += 1
    if index != len(frames):
        raise RekeyError(f"found {index} `hex:` lines but the capture has {len(frames)} frames")

    key_lines = [n for n, line in enumerate(lines) if _KEY_LINE_RE.match(line.rstrip("\n"))]
    if len(key_lines) != 1:
        raise RekeyError(f"expected exactly one top-level `key:` line, found {len(key_lines)}")
    lines[key_lines[0]] = "key: corpus\n"
    return "".join(lines), changed


def assert_output_is_clean(text: str, capture: dict, real_key: bytes) -> None:
    """Last-line checks on what is about to be committed: the real key must not survive anywhere
    in the file — not in the frames (ingest.assert_no_key_leakage), and not as a hex string in a
    description or note someone pasted it into — and the result must satisfy validate.py's own
    crypto enforcement, so a bad re-key fails here rather than in `make lint`.
    """
    frames = load_frames(capture)
    ingest.assert_no_key_leakage(frames, real_key)

    haystack = re.sub(r"[^0-9a-f]", "", text.lower())
    for candidate in (real_key, bytes(reversed(real_key))):
        if candidate.hex() in haystack:
            raise RekeyError("OUTPUT-SAFETY ABORT: the real key's hex appears in the output text (a description "
                             "or note?) — refusing to write")

    validate.validate_crypto(capture, capture.get("id", "<no id>"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", help="capture YAML still holding real key material (must not be committable)")
    parser.add_argument("-o", "--output", required=True, help="path to write the re-keyed capture to")
    parser.add_argument("--system-key-from", default=None,
                        help="path to a git-ignored file holding the real system key (32 hex digits) — for captures "
                             "with no 0x32 frame to recover it from")
    args = parser.parse_args()

    input_path = Path(args.input)
    output_path = Path(args.output)
    try:
        if input_path.resolve() == output_path.resolve():
            raise RekeyError("--output must differ from the input — the pre-re-key file is deleted by hand, "
                             "not overwritten in place, so a failed run can never leave a half-rewritten capture")
        assert_input_is_not_committable(input_path)

        text = input_path.read_text(encoding="utf-8")
        capture = yaml.safe_load(text)
        frames = load_frames(capture)
        original_hex = [frame.hex_bytes for frame in frames]

        real_key, key_source = resolve_key(frames, args.system_key_from)
        hmac_count = ingest.rekey_hmac_frames(frames, real_key, protolib.CORPUS_SYSTEM_KEY)
        hmac_count += ingest.rekey_self_authenticated_frames(frames, real_key, protolib.CORPUS_SYSTEM_KEY)
        transfer_count = ingest.rekey_key_transfer_frames(frames, real_key, protolib.CORPUS_SYSTEM_KEY)
        ingest.assert_no_key_leakage(frames, real_key)

        rendered, changed = rewrite_text(text, frames, original_hex)
        assert_output_is_clean(rendered, yaml.safe_load(rendered), real_key)
    except RekeyError as exc:
        print(f"rekey_capture.py: ABORT: {exc}", file=sys.stderr)
        return 1
    except validate.ValidationError as exc:
        print(f"rekey_capture.py: ABORT: re-keyed output fails validate.py's crypto enforcement: {exc}",
              file=sys.stderr)
        return 1

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(rendered, encoding="utf-8")
    print(f"rekey_capture.py: real key taken from {key_source} (fingerprint sha256={key_fingerprint(real_key)}, "
          f"never printed or stored), verified+rewrote {transfer_count} key-transfer frame(s) and {hmac_count} "
          f"HMAC frame(s) under the corpus key (fingerprint sha256={key_fingerprint(protolib.CORPUS_SYSTEM_KEY)})")
    print(f"rekey_capture.py: wrote {output_path} ({changed} frame(s) rewritten, key: corpus)")

    # The bytes are fixed but prose written when the capture was `key: unknown` now lies. Only the
    # human can rewrite a note, so say so loudly rather than leaving a contradiction in the file.
    stale = [phrase for phrase in ("key unknown", "not verifiable", "unverifiable", "key: unknown")
             if phrase in rendered.lower()]
    if stale:
        print(f"rekey_capture.py: REVIEW {output_path}: its text still says {stale} — that was true before this "
              "re-key and is not now. Update those notes/description before committing.", file=sys.stderr)
    print(f"rekey_capture.py: now delete {input_path} — it still contains the real key material")
    return 0


if __name__ == "__main__":
    sys.exit(main())
