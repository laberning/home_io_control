#!/usr/bin/env python3
"""Schema and self-consistency validation for the golden-frame corpus.

Checks every capture YAML under tests/corpus/captures/**/*.yaml:
  * required fields present, `key`, `source.origin`, and `source.captured_with` hold allowed
    values;
  * `hex` is well-formed (even count of hex digits, whitespace-tolerant);
  * frame byte count is >= FRAME_MIN_SIZE (+2 when `crc: present`);
  * CTRL0 length bits (bits [4:0] = frame_length - 1, CRC bytes not counted) agree with the
    non-CRC byte count — mirrors proto_frame.cpp parse()/frame_length();
  * CRC-CCITT (poly 0x1021 reversed = 0x8408, init 0x0000) matches the trailing 2 bytes where
    `crc: present` — ported from crc_ccitt() in proto_frame.cpp;
  * duplicate `id` across the whole corpus is a hard error;
  * `expect.*` keys are restricted to the set build.py actually consumes, and `expect.frames`
    cannot have more entries than were captured — both are otherwise silently accepted by
    build.py's dict.get()/index-bounded lookups, defeating the "expectations are
    human-verified" rule (see tests/corpus/README.md).

`key: corpus` cryptographic promises are enforced: every 0x3C/0x3D HMAC and 0x31/0x3C/0x32
key-transfer payload in a `key: corpus` capture must verify/decrypt correctly under the public
corpus key (protolib.CORPUS_SYSTEM_KEY, mirroring tests/support/test_helpers.h ::
TEST_SYSTEM_KEY). Independently of `key:`, ANY capture (any key mode) containing a 0x32
key-transfer frame whose payload does not decrypt to the corpus key is a hard failure — this is
the safety net that stops an un-re-keyed raw pairing capture (which would leak a real system
key) from ever being committed, regardless of what its `key:` field claims (design §4.3).

Run via `make corpus-validate` (part of the `lint` composite). Dependencies: PyYAML, cryptography.
"""

import sys
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build import CLASSIFICATION_ENUM  # noqa: E402
import protolib  # noqa: E402
from protolib import CTRL0_LENGTH_MASK, FRAME_MAX_SIZE, FRAME_MIN_SIZE, HMAC_SIZE, crc_ccitt  # noqa: E402
from protolib import parse_hex as protolib_parse_hex  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
CAPTURES_DIR = REPO_ROOT / "tests" / "corpus" / "captures"

ALLOWED_KEY_MODES = {"corpus", "unknown"}
ALLOWED_ORIGINS = {"own-hardware", "github-issue", "synthetic-bootstrap", "reference-material"}
# Mirrors ingest.py's --captured-with choices — build.py hard-requires source.captured_with
# (render_capture() reads capture["source"]["captured_with"] unconditionally) but validate.py
# didn't check it was present, so a hand-edited or older capture missing it passed validation
# and then died in build.py with a raw KeyError traceback instead of a readable message.
ALLOWED_CAPTURED_WITH = {"sx1276", "sx1262", "other", "synthetic"}
ALLOWED_DIRS = {"tx", "rx"}
ALLOWED_CRC = {"present", "absent"}

# --- Mirrors the expect-schema keys scripts/corpus/build.py actually consumes -------------
# A key outside these sets is silently ignored by build.py (dict.get() just returns None), so
# a typo'd expectation (e.g. `cmdd:` instead of `cmd:`) would never be checked despite the human
# believing they wrote a verified expectation. Fail loudly here instead.
ALLOWED_EXPECT_KEYS = {"frames", "exchange", "device", "oneway"}
ALLOWED_EXPECT_FRAME_KEYS = {"cmd", "start", "end", "protocol", "classification", "hmac_valid"}
ALLOWED_EXPECT_EXCHANGE_KEYS = {"kind", "outcome"}
ALLOWED_EXPECT_DEVICE_KEYS = {"reported_position", "reported_tilt", "name"}
ALLOWED_EXPECT_ONEWAY_KEYS = {"intent", "target_type", "originator", "acei"}

# Names only — imported from build.py::CLASSIFICATION_ENUM rather than hand-mirrored, so a new
# disposition name needs one coordinated edit (there, plus the static_assert in
# corpus_classification_test.cpp) instead of two. validate.py only needs to reject typos before
# they reach build.py's own hard-error; the numeric mapping stays in build.py.
ALLOWED_CLASSIFICATIONS = set(CLASSIFICATION_ENUM)


class ValidationError(Exception):
    """A single capture failed validation; carries a human-readable message."""


def parse_hex(hex_str: str, context: str) -> bytes:
    """Thin, error-context-carrying adapter over protolib.parse_hex (the canonical implementation)."""
    compact = "".join(hex_str.split())
    if len(compact) % 2 != 0:
        raise ValidationError(f"{context}: hex string has an odd number of digits: {hex_str!r}")
    try:
        return protolib_parse_hex(hex_str)
    except ValueError as exc:
        raise ValidationError(f"{context}: hex string is not valid hex: {hex_str!r} ({exc})") from exc


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValidationError(message)


def validate_frame(frame: dict, index: int, capture_id: str) -> None:
    context = f"{capture_id} frame[{index}]"
    for field in ("dir", "hex", "crc"):
        require(field in frame, f"{context}: missing required field '{field}'")

    require(frame["dir"] in ALLOWED_DIRS, f"{context}: dir must be one of {sorted(ALLOWED_DIRS)}, got {frame['dir']!r}")
    require(frame["crc"] in ALLOWED_CRC, f"{context}: crc must be one of {sorted(ALLOWED_CRC)}, got {frame['crc']!r}")

    raw = parse_hex(frame["hex"], context)
    crc_present = frame["crc"] == "present"
    non_crc_len = len(raw) - 2 if crc_present else len(raw)

    require(
        non_crc_len >= FRAME_MIN_SIZE,
        f"{context}: frame is {non_crc_len} bytes (excluding CRC), below FRAME_MIN_SIZE={FRAME_MIN_SIZE}",
    )
    # A frame's non-CRC bytes are either exactly the CTRL0-declared length, or that length plus
    # the out-of-length MAC trailer some 1W frames carry (a CMD 0x30 add-controller payload
    # overflows CTRL0's 5-bit length field alongside its MAC — see
    # reference_1w_vectors/oneway_add_controller_kat.yaml — so the MAC rides after the declared
    # length instead, mirrored by proto_frame.cpp parse()'s two accepted shapes, IoFrame::has_mac).
    # The declared length alone can never exceed FRAME_MAX_SIZE (CTRL0's 5-bit field), so the
    # widest legal non_crc_len is FRAME_MAX_SIZE + HMAC_SIZE; this still hard-rejects a genuinely
    # oversized or otherwise-malformed frame rather than going blind to it.
    max_non_crc_len = FRAME_MAX_SIZE + HMAC_SIZE
    require(
        non_crc_len <= max_non_crc_len,
        f"{context}: frame is {non_crc_len} bytes (excluding CRC), above FRAME_MAX_SIZE+HMAC_SIZE={max_non_crc_len}",
    )

    ctrl0 = raw[0]
    expected_declared = (ctrl0 & CTRL0_LENGTH_MASK) + 1
    has_mac_trailer = non_crc_len == expected_declared + HMAC_SIZE
    require(
        expected_declared == non_crc_len or has_mac_trailer,
        f"{context}: CTRL0 length bits imply {expected_declared} bytes (or {expected_declared + HMAC_SIZE} bytes "
        f"with a MAC trailer), but captured non-CRC length is {non_crc_len}",
    )

    if crc_present:
        payload = raw[:non_crc_len]
        computed = crc_ccitt(payload)
        captured_crc = raw[non_crc_len] | (raw[non_crc_len + 1] << 8)
        require(
            computed == captured_crc,
            f"{context}: CRC mismatch — computed 0x{computed:04X}, captured 0x{captured_crc:04X}",
        )


def require_known_keys(mapping: dict, allowed: set, context: str) -> None:
    unknown = set(mapping.keys()) - allowed
    require(not unknown, f"{context}: unknown key(s) {sorted(unknown)}; expected one of {sorted(allowed)}")


def validate_expect(expect: dict, frame_count: int, capture_id: str) -> None:
    """Fail loudly on unknown expect.* keys and on more expect.frames entries than were captured
    — both are silently accepted by build.py today, defeating the "expectations are
    human-verified" rule (the extra/misspelled expectation is simply never checked).
    """
    require(isinstance(expect, dict), f"{capture_id}: 'expect' must be a mapping")
    require_known_keys(expect, ALLOWED_EXPECT_KEYS, f"{capture_id}: expect")

    expect_frames = expect.get("frames")
    if expect_frames is not None:
        require(isinstance(expect_frames, list), f"{capture_id}: expect.frames must be a list")
        require(
            len(expect_frames) <= frame_count,
            f"{capture_id}: expect.frames has {len(expect_frames)} entries but only {frame_count} "
            "frame(s) were captured — the extra entries would be silently ignored by build.py",
        )
        for index, expect_frame in enumerate(expect_frames):
            require(isinstance(expect_frame, dict), f"{capture_id}: expect.frames[{index}] must be a mapping")
            require_known_keys(expect_frame, ALLOWED_EXPECT_FRAME_KEYS, f"{capture_id}: expect.frames[{index}]")
            if "classification" in expect_frame:
                require(
                    expect_frame["classification"] in ALLOWED_CLASSIFICATIONS,
                    f"{capture_id}: expect.frames[{index}].classification must be one of "
                    f"{sorted(ALLOWED_CLASSIFICATIONS)}, got {expect_frame['classification']!r}",
                )

    exchange = expect.get("exchange")
    if exchange is not None:
        require(isinstance(exchange, dict), f"{capture_id}: expect.exchange must be a mapping")
        require_known_keys(exchange, ALLOWED_EXPECT_EXCHANGE_KEYS, f"{capture_id}: expect.exchange")

    device = expect.get("device")
    if device is not None:
        require(isinstance(device, dict), f"{capture_id}: expect.device must be a mapping")
        require_known_keys(device, ALLOWED_EXPECT_DEVICE_KEYS, f"{capture_id}: expect.device")

    oneway = expect.get("oneway")
    if oneway is not None:
        require(isinstance(oneway, dict), f"{capture_id}: expect.oneway must be a mapping")
        require_known_keys(oneway, ALLOWED_EXPECT_ONEWAY_KEYS, f"{capture_id}: expect.oneway")


def _non_crc_raw_frame(frame: dict) -> "protolib.RawFrame":
    """Wrap a YAML frame dict as a protolib.RawFrame holding exactly the non-CRC bytes, so byte
    offsets (cmd, data, HMAC/key-transfer payload) line up regardless of `crc: present/absent` —
    reuses protolib's own triple-finders instead of re-deriving frame shape logic here.
    """
    raw = bytes.fromhex("".join(frame["hex"].split()))
    if frame.get("crc") == "present":
        raw = raw[:-2]
    return protolib.RawFrame(frame["dir"], raw.hex())


def validate_crypto(data: dict, capture_id: str) -> None:
    """Crypto enforcement — see module docstring. Frames are already known well-formed by the
    time this runs (validate_frame ran first), so CTRL0-implied length == actual length holds.
    """
    frames = [_non_crc_raw_frame(frame) for frame in data["frames"]]

    for key_init, challenge, transfer in protolib.find_key_transfer_triples(frames):
        challenge_raw = challenge.raw()
        transfer_raw = transfer.raw()
        require(
            len(challenge_raw) >= FRAME_MIN_SIZE + protolib.HMAC_SIZE,
            f"{capture_id}: 0x3C challenge frame preceding a 0x32 key-transfer is shorter than expected",
        )
        require(
            len(transfer_raw) >= FRAME_MIN_SIZE + protolib.AES_KEY_SIZE,
            f"{capture_id}: 0x32 key-transfer frame payload is shorter than {protolib.AES_KEY_SIZE} bytes",
        )
        parts = protolib.key_transfer_parts(key_init, challenge, transfer)
        decrypted = protolib.crypt_key(parts.key_init_cmd, parts.challenge, parts.encrypted_payload)
        require(
            decrypted == protolib.CORPUS_SYSTEM_KEY,
            f"{capture_id}: SECURITY: a 0x32 key-transfer frame's payload does not decrypt to the public "
            "corpus key — this capture must be re-keyed (scripts/corpus/ingest.py --rekey) before it can "
            "ever be committed; committing it as-is would leak whatever real key it currently encodes",
        )

    if data["key"] != "corpus":
        return

    for origin, challenge, response in protolib.find_challenge_response_triples(frames):
        challenge_raw = challenge.raw()
        response_raw = response.raw()
        require(
            len(challenge_raw) >= FRAME_MIN_SIZE + protolib.HMAC_SIZE,
            f"{capture_id}: 0x3C challenge frame is shorter than expected",
        )
        require(
            len(response_raw) >= FRAME_MIN_SIZE + protolib.HMAC_SIZE,
            f"{capture_id}: 0x3D challenge-response frame payload is shorter than {protolib.HMAC_SIZE} bytes",
        )
        parts = protolib.hmac_parts(origin, challenge, response)
        require(
            protolib.verify_hmac(parts.transcript, parts.hmac, parts.challenge, protolib.CORPUS_SYSTEM_KEY),
            f"{capture_id}: key: corpus promise broken — a 0x3D HMAC does not verify under the public "
            "corpus key",
        )

    # Self-authenticated frames (0x2A: payload = [challenge | HMAC]) carry a complete
    # challenge-response with no surrounding frames to pair them with, so the loop above cannot
    # see them — they are opaque blobs to a reader and were, until a real capture turned up,
    # invisible to this enforcement too.
    for frame in protolib.find_self_authenticated_frames(frames):
        transcript, self_challenge, self_hmac = protolib.self_auth_parts(frame)
        require(
            protolib.verify_hmac(transcript, self_hmac, self_challenge, protolib.CORPUS_SYSTEM_KEY),
            f"{capture_id}: key: corpus promise broken — the HMAC half of a self-authenticated "
            f"0x{protolib.CMD_DISCOVER_SPE_REQ:02X} payload does not verify under the public corpus key "
            "(re-key it with scripts/corpus/ingest.py --rekey or rekey_capture.py)",
        )


def validate_capture(data: dict, path: Path) -> str:
    for field in ("id", "description", "source", "key", "frames"):
        require(field in data, f"{path}: missing required top-level field '{field}'")

    capture_id = data["id"]
    require(isinstance(capture_id, str) and capture_id, f"{path}: 'id' must be a non-empty string")

    require(data["key"] in ALLOWED_KEY_MODES, f"{capture_id}: key must be one of {sorted(ALLOWED_KEY_MODES)}")

    source = data["source"]
    require(isinstance(source, dict), f"{capture_id}: 'source' must be a mapping")
    for field in ("origin", "captured_with", "device", "date"):
        require(field in source, f"{capture_id}: source.{field} is required")
    require(
        source["origin"] in ALLOWED_ORIGINS,
        f"{capture_id}: source.origin must be one of {sorted(ALLOWED_ORIGINS)}, got {source['origin']!r}",
    )
    require(
        source["captured_with"] in ALLOWED_CAPTURED_WITH,
        f"{capture_id}: source.captured_with must be one of {sorted(ALLOWED_CAPTURED_WITH)}, "
        f"got {source['captured_with']!r}",
    )

    frames = data["frames"]
    require(isinstance(frames, list) and len(frames) > 0, f"{capture_id}: 'frames' must be a non-empty list")
    for index, frame in enumerate(frames):
        validate_frame(frame, index, capture_id)

    if "expect" in data:
        validate_expect(data["expect"], len(frames), capture_id)

    validate_crypto(data, capture_id)

    return capture_id


def main() -> int:
    if not CAPTURES_DIR.is_dir():
        print(f"error: captures directory not found: {CAPTURES_DIR}", file=sys.stderr)
        return 1

    paths = sorted(CAPTURES_DIR.rglob("*.yaml"))
    if not paths:
        print("validate.py: no capture YAML files found (nothing to validate)")
        return 0

    seen_ids: dict = {}
    errors: list = []
    total_frames = 0

    for path in paths:
        try:
            with path.open(encoding="utf-8") as handle:
                data = yaml.safe_load(handle)
            capture_id = validate_capture(data, path)
        except ValidationError as exc:
            errors.append(str(exc))
            continue
        except yaml.YAMLError as exc:
            errors.append(f"{path}: YAML parse error: {exc}")
            continue

        if capture_id in seen_ids:
            errors.append(f"{path}: duplicate id '{capture_id}' (already used by {seen_ids[capture_id]})")
        else:
            seen_ids[capture_id] = path
            total_frames += len(data["frames"])

    if errors:
        print("validate.py: FAILED", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1

    print(f"validate.py: OK ({len(seen_ids)} captures, {total_frames} frames)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
