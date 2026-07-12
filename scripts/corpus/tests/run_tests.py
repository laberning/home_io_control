#!/usr/bin/env python3
"""Self-tests for the golden-frame corpus toolchain (protolib.py / ingest.py / validate.py).

Stdlib `assert`-based, no pytest — mirrors scripts/check-tuning-sync.py's dependency-light
style. Fixtures under scripts/corpus/tests/data/ are trimmed excerpts of the real
analysis/issues/*.txt logs (plus one synthetic mangled paste exercising the fallback tier).

Run via `python3 scripts/corpus/tests/run_tests.py`; wired into `make corpus-validate`.
Exits non-zero with a description of the first failure.
"""

import copy
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace

import yaml

REPO_ROOT = Path(__file__).resolve().parent.parent.parent.parent
SCRIPTS_DIR = REPO_ROOT / "scripts" / "corpus"
DATA_DIR = Path(__file__).resolve().parent / "data"

sys.path.insert(0, str(SCRIPTS_DIR))
import build as build_module  # noqa: E402
import ingest as ingest_module  # noqa: E402
import protolib  # noqa: E402
import validate as validate_module  # noqa: E402


def _read(name: str) -> str:
    return (DATA_DIR / name).read_text(encoding="utf-8")


def test_clean_io_capture_sx1262() -> None:
    frames = protolib.parse_log(_read("clean_io_capture_sx1262.txt"))
    assert len(frames) == 2, f"expected 2 frames, got {len(frames)}"
    assert [f.direction for f in frames] == ["rx", "rx"]
    assert not any(f.unverified for f in frames), "clean io_capture lines must not be unverified"
    expected0 = bytes.fromhex("96 00 26 94 11 E6 73 34 04 05 00 00 00 C8 00 00 19 E6 73 34 00 00 00".replace(" ", ""))
    assert frames[0].raw() == expected0, f"frame0 bytes mismatch: {frames[0].raw().hex()}"
    assert frames[0].freq == 868250000
    assert frames[0].chip == "sx1262"
    assert not frames[0].crc_present(), "23-byte payload matches CTRL0 length exactly (0x96 -> 23) — no CRC"

    expected1 = bytes.fromhex("11 B3 A1 98 44 05 00 02 00 C8 00 00 00 B3 A1 98 10 00".replace(" ", ""))
    assert frames[1].raw() == expected1, f"frame1 bytes mismatch: {frames[1].raw().hex()}"
    assert frames[1].freq == 869850000


def test_io_frame_legacy_dedup_and_no_cmd_field() -> None:
    frames = protolib.parse_log(_read("io_frame_legacy.txt"))
    # The io_capture tx_frame + io_frame TX lines describe the same physical frame and must
    # merge into one; the standalone io_frame RX line (no cmd= field, older firmware shape) is
    # a second, distinct frame.
    assert len(frames) == 2, f"expected 2 frames after dedup, got {len(frames)}"
    tx, rx = frames
    assert tx.direction == "tx"
    assert rx.direction == "rx"
    expected_tx = bytes.fromhex("50 20 7F 59 58 31 BA F7 00 01 E7 D4 00 20 32 00 00".replace(" ", ""))
    assert tx.raw() == expected_tx, f"tx bytes mismatch: {tx.raw().hex()}"
    assert tx.freq == 868950000, "merge must keep the (agreeing) freq from either side"
    assert tx.chip == "sx1276", "merge must keep chip from the io_capture side (io_frame has none)"
    assert not tx.unverified

    expected_rx = bytes.fromhex("0E 00 31 BA F7 7F 59 58 3C EE 4B 9D FE 53 07".replace(" ", ""))
    assert rx.raw() == expected_rx, f"rx bytes mismatch: {rx.raw().hex()}"
    assert not rx.unverified, "the no-cmd= io_frame shape must still be recognized (not fallback tier)"
    assert not rx.crc_present(), "15-byte payload matches CTRL0 length exactly (0x0E -> 15) — no CRC"


def test_no_crc_write_private() -> None:
    frames = protolib.parse_log(_read("no_crc_write_private.txt"))
    assert len(frames) == 1
    frame = frames[0]
    expected = bytes.fromhex(
        "F5 00 00 00 3F 2F 9A 98 20 02 03 05 02 00 0F 84 FF FC 46 0E AC 87".replace(" ", "")
    )
    assert frame.raw() == expected, f"bytes mismatch: {frame.raw().hex()}"
    assert len(frame.raw()) == protolib.ctrl0_implied_length(frame.raw()[0]) == 22
    assert not frame.crc_present(), "22-byte payload matches CTRL0 length exactly (0xF5 -> 22) — no CRC"


def test_mangled_paste_fallback_tier() -> None:
    frames = protolib.parse_log(_read("mangled_paste.txt"))
    assert len(frames) == 1, f"expected exactly 1 recovered frame, got {len(frames)}"
    frame = frames[0]
    assert frame.unverified, "fallback-tier extraction must be marked unverified"
    expected = bytes.fromhex("50 20 7F 59 58 31 BA F7 00 01 E7 D4 00 20 32 00 00".replace(" ", ""))
    assert frame.raw() == expected, f"fallback extraction bytes mismatch: {frame.raw().hex()}"


def test_io_frame_only_retry_not_merged() -> None:
    # Regression for a false merge found ingesting a real io_capture-disabled log: two
    # genuinely distinct io_frame-only TX lines with identical bytes (a retried PRIVATE status
    # poll retransmitting the exact same request after a first-response timeout) must stay two
    # frames, not collapse into one via the io_capture/io_frame dedup heuristic.
    frames = protolib.parse_log(_read("io_frame_only_retry.txt"))
    assert len(frames) == 2, f"expected 2 distinct retry frames, got {len(frames)}"
    assert frames[0].direction == frames[1].direction == "tx"
    assert frames[0].raw() == frames[1].raw(), "both retries carry the identical request payload"
    assert frames[0].chip is None and frames[1].chip is None, "io_frame-only lines never carry chip"


def test_merge_prefers_nonzero_freq_and_t_ms() -> None:
    # Reproduces the observed firmware quirk (analysis/issues/27.txt): a retried DISCOVER_REQ's
    # io_capture tx_frame entry logs freq=0/ts=0 (no capture context at that call site), while
    # the paired io_frame entry for the exact same transmission carries the real freq.
    a = protolib.RawFrame("tx", "C8 00 00 00 3B C0 FF EE 28", freq=0, t_ms=0, chip="sx1262", unverified=False)
    b = protolib.RawFrame("tx", "C8 00 00 00 3B C0 FF EE 28", freq=868950000, t_ms=1234, chip=None, unverified=False)
    assert protolib._same_physical_frame(a, b)
    merged = protolib._merge_frames(a, b)
    assert merged.freq == 868950000
    assert merged.t_ms == 1234
    assert merged.chip == "sx1262"


def test_crc_ccitt_matches_known_vector() -> None:
    # Cross-check against tests/corpus/captures/_bootstrap/synthetic_1w_close.yaml, whose bytes
    # were generated from the real C++ crc_ccitt() (tests/corpus_bootstrap_dump_test.cpp).
    payload = bytes.fromhex("EC 00 00 00 BF AA BB CC 00 01 41 C8 00".replace(" ", ""))
    assert protolib.crc_ccitt(payload) == 0x7E35, f"crc mismatch: 0x{protolib.crc_ccitt(payload):04X}"


def _valid_capture_dict() -> dict:
    """A minimal, self-consistent capture — the same bytes as
    tests/corpus/captures/_bootstrap/synthetic_1w_close.yaml (real crc_ccitt() output, see
    test_crc_ccitt_matches_known_vector above), as a plain dict for validate_capture()/
    render_frame(), which both take dicts directly — no temp files needed for schema tests.
    """
    return {
        "id": "self_test_valid",
        "description": "schema self-test fixture",
        "source": {
            "origin": "synthetic-bootstrap",
            "captured_with": "synthetic",
            "device": "self-test fixture",
            "date": "2026-07-06",
        },
        "key": "unknown",
        "frames": [
            {
                "dir": "rx",
                "freq": 868950000,
                "hex": "EC 00 00 00 BF AA BB CC 00 01 41 C8 00 35 7E",
                "crc": "present",
            }
        ],
        "expect": {
            "frames": [{"cmd": 0x00, "start": True, "end": True, "protocol": "1w"}],
        },
    }


def _assert_validation_fails(data: dict, needle: str) -> None:
    try:
        validate_module.validate_capture(data, Path("<self-test>"))
    except validate_module.ValidationError as exc:
        assert needle in str(exc), f"expected {needle!r} in error, got: {exc}"
        return
    raise AssertionError(f"expected ValidationError containing {needle!r}, but validation passed")


def test_validate_ctrl0_length_mismatch_is_rejected() -> None:
    data = copy.deepcopy(_valid_capture_dict())
    # 0xEC -> 0xED changes the CTRL0 length bits (0x0C -> 0x0D) without changing the byte count.
    data["frames"][0]["hex"] = "ED 00 00 00 BF AA BB CC 00 01 41 C8 00 35 7E"
    _assert_validation_fails(data, "CTRL0 length bits")


def test_validate_crc_mismatch_is_rejected() -> None:
    data = copy.deepcopy(_valid_capture_dict())
    data["frames"][0]["hex"] = "EC 00 00 00 BF AA BB CC 00 01 41 C8 00 00 00"
    _assert_validation_fails(data, "CRC mismatch")


def test_validate_unknown_expect_key_is_rejected() -> None:
    data = copy.deepcopy(_valid_capture_dict())
    data["expect"]["not_a_real_expect_key"] = True
    _assert_validation_fails(data, "unknown key(s)")


def test_validate_over_length_expect_frames_is_rejected() -> None:
    data = copy.deepcopy(_valid_capture_dict())
    data["expect"]["frames"].append({"cmd": 0x01})
    _assert_validation_fails(data, "expect.frames has")


def test_validate_bad_classification_name_is_rejected() -> None:
    data = copy.deepcopy(_valid_capture_dict())
    data["expect"]["frames"][0]["classification"] = "NOT_A_REAL_DISPOSITION"
    _assert_validation_fails(data, "classification must be one of")


def test_build_partial_flag_expectation_is_rejected() -> None:
    # build.py's own hard-error: start/end/protocol must be specified all together
    # or not at all — a partial set would silently assert false for the missing two.
    frame = {"dir": "rx", "hex": "EC 00 00 00 BF AA BB CC 00 01 41 C8 00", "crc": "absent"}
    try:
        build_module.render_frame("self_test", 0, frame, {"start": True})
    except SystemExit as exc:
        assert "partial flag expectation" in str(exc)
        return
    raise AssertionError("expected build.py to reject a partial start/end/protocol expectation")


def test_build_output_is_deterministic() -> None:
    # Invariant 0.5.5: build.py's output must be byte-identical across runs (no timestamps, no
    # unordered iteration) — rendering the real corpus twice must produce the same bytes.
    captures = build_module.load_captures()
    first = build_module.render(captures)
    second = build_module.render(captures)
    assert first == second, "build.py render() must be byte-identical across repeated runs"


def test_end_to_end_scaffold_validate_build_roundtrip() -> None:
    """ingest.py on a fixture log -> validate.py on its output passes -> build.py compiles it.

    Uses a throwaway temp directory (auto-cleaned) — this output is a tool self-test artifact,
    never corpus material.
    """
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        captures_dir = tmp_path / "captures"
        captures_dir.mkdir()
        output_yaml = captures_dir / "scaffold_smoke.yaml"

        argv = [
            str(DATA_DIR / "clean_io_capture_sx1262.txt"),
            "--id",
            "scaffold_smoke",
            "--device",
            "self-test fixture",
            "--captured-with",
            "sx1262",
            "--origin",
            "github-issue",
            "--issue",
            "https://github.com/laberning/home_io_control/issues/3",
            "--date",
            "2026-07-06",
            "-o",
            str(output_yaml),
        ]
        rc = _run_ingest_main(argv)
        assert rc == 0, "ingest.py should exit 0 on a well-formed fixture"
        assert output_yaml.is_file(), "ingest.py must write the requested output file"

        original_captures_dir = validate_module.CAPTURES_DIR
        try:
            validate_module.CAPTURES_DIR = captures_dir
            assert validate_module.main() == 0, "validate.py must accept the scaffolded capture"
        finally:
            validate_module.CAPTURES_DIR = original_captures_dir

        original_build_captures_dir = build_module.CAPTURES_DIR
        original_output_path = build_module.OUTPUT_PATH
        try:
            build_module.CAPTURES_DIR = captures_dir
            build_module.OUTPUT_PATH = tmp_path / "corpus_generated.h"
            assert build_module.main() == 0, "build.py must compile the scaffolded capture"
            assert build_module.OUTPUT_PATH.is_file()
            generated = build_module.OUTPUT_PATH.read_text(encoding="utf-8")
            assert "scaffold_smoke" in generated
        finally:
            build_module.CAPTURES_DIR = original_build_captures_dir
            build_module.OUTPUT_PATH = original_output_path
    # tmp (and the throwaway scaffold/generated header) is deleted on context-manager exit.


def _run_ingest_main(argv: "list[str]") -> int:
    old_argv = sys.argv
    try:
        sys.argv = ["ingest.py"] + argv
        return ingest_module.main()
    finally:
        sys.argv = old_argv


def test_crypto_kat_vectors_match_cpp() -> None:
    """Python port (protolib.create_hmac) must reproduce the vectors generated from the real
    C++ implementation (tests/corpus_bootstrap_dump_test.cpp :: DISABLED_PrintCryptoKatVectors),
    pinned again as a hardcoded C++ test in tests/corpus_crypto_test.cpp. A divergence between
    the two implementations fails this gate.
    """
    kat_path = SCRIPTS_DIR / "tests" / "data" / "crypto_kat.yaml"
    with kat_path.open(encoding="utf-8") as handle:
        kat = yaml.safe_load(handle)
    key = bytes.fromhex(kat["key"].replace(" ", ""))
    challenge = bytes.fromhex(kat["challenge"].replace(" ", ""))
    assert key == protolib.CORPUS_SYSTEM_KEY, "crypto_kat.yaml key must match protolib.CORPUS_SYSTEM_KEY"
    for vector in kat["vectors"]:
        data = bytes.fromhex(vector["data"].replace(" ", ""))
        expected_hmac = bytes.fromhex(vector["hmac"].replace(" ", ""))
        got = protolib.create_hmac(data, challenge, key)
        assert got == expected_hmac, f"{vector['name']}: got {got.hex().upper()}, expected {expected_hmac.hex().upper()}"


# --- --rekey pipeline self-tests -------------------------------------------------------------

_REKEY_CONTROLLER_ID = bytes.fromhex("112233")
_REKEY_DEVICE_ID = bytes.fromhex("AABBCC")


def _make_raw_frame(direction: str, ctrl0_flags: int, dst: bytes, src: bytes, cmd: int, data: bytes) -> "protolib.RawFrame":
    total_len = protolib.FRAME_MIN_SIZE + len(data)
    ctrl0 = (ctrl0_flags & 0xE0) | ((total_len - 1) & 0x1F)
    raw = bytes([ctrl0, 0x00]) + dst + src + bytes([cmd]) + data
    return protolib.RawFrame(direction, raw.hex())


def _build_synthetic_exchange(real_key: bytes) -> "list[protolib.RawFrame]":
    """tx EXECUTE-shaped origin -> rx 0x3C challenge -> tx 0x3D response, HMAC computed under
    `real_key` exactly as create_challenge_resp() would (proto_commands.cpp): transcript =
    [origin.cmd] + origin.data.
    """
    origin_data = bytes([0x01, 0x02, 0x03])
    origin = _make_raw_frame("tx", 0x40, _REKEY_DEVICE_ID, _REKEY_CONTROLLER_ID, 0x00, origin_data)
    challenge_bytes = bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0x66])
    challenge = _make_raw_frame("rx", 0x00, _REKEY_CONTROLLER_ID, _REKEY_DEVICE_ID, protolib.CMD_CHALLENGE_REQ,
                                challenge_bytes)
    transcript = bytes([0x00]) + origin_data
    real_hmac = protolib.create_hmac(transcript, challenge_bytes, real_key)
    response = _make_raw_frame("tx", 0x00, _REKEY_DEVICE_ID, _REKEY_CONTROLLER_ID, protolib.CMD_CHALLENGE_RESP,
                               real_hmac)
    return [origin, challenge, response]


def _write_fake_key_file(key: bytes) -> Path:
    """Writes a scratch key file under build/ (git-ignored, confirmed by --rekey's own check)
    so the self-test can exercise --system-key-from's git-ignore refusal honestly rather than
    special-casing it away.
    """
    path = REPO_ROOT / "build" / "corpus" / "_test_rekey_key.txt"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(key.hex(), encoding="utf-8")
    return path


def test_rekey_happy_path_rewrites_hmac_under_corpus_key() -> None:
    real_key = bytes([0xA5] * 16)
    frames = _build_synthetic_exchange(real_key)
    key_path = _write_fake_key_file(real_key)
    try:
        args = SimpleNamespace(system_key_from=str(key_path), remap=[], role=[])
        returned_key, warnings, node_map = ingest_module.apply_rekey(frames, args)
        assert returned_key == real_key
        assert warnings == []
        assert node_map == {}

        response_raw = frames[2].raw()
        rewritten_hmac = response_raw[protolib.FRAME_MIN_SIZE:protolib.FRAME_MIN_SIZE + protolib.HMAC_SIZE]
        origin_data = bytes([0x01, 0x02, 0x03])
        transcript = bytes([0x00]) + origin_data
        challenge_bytes = bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0x66])
        assert protolib.verify_hmac(transcript, rewritten_hmac, challenge_bytes, protolib.CORPUS_SYSTEM_KEY), (
            "rekeyed HMAC must verify under the corpus key")
        assert not protolib.verify_hmac(transcript, rewritten_hmac, challenge_bytes, real_key), (
            "rekeyed HMAC must NOT still verify under the real key — it must have been rewritten, not just checked")
    finally:
        key_path.unlink(missing_ok=True)


def test_rekey_tamper_abort() -> None:
    """A captured HMAC that does not verify under the resolved real key must hard-abort —
    never silently pass through or produce output.
    """
    real_key = bytes([0xA5] * 16)
    frames = _build_synthetic_exchange(real_key)
    response_raw = bytearray(frames[2].raw())
    response_raw[-1] ^= 0xFF  # corrupt the last HMAC byte
    frames[2].hex_bytes = bytes(response_raw).hex()

    key_path = _write_fake_key_file(real_key)
    try:
        args = SimpleNamespace(system_key_from=str(key_path), remap=[], role=[])
        try:
            ingest_module.apply_rekey(frames, args)
            raise AssertionError("expected RekeyError on a tampered/corrupted captured HMAC")
        except ingest_module.RekeyError:
            pass
    finally:
        key_path.unlink(missing_ok=True)


def test_rekey_refuses_non_gitignored_key_path() -> None:
    args = SimpleNamespace(system_key_from=str(REPO_ROOT / "README.md"), remap=[], role=[])
    try:
        ingest_module.resolve_system_key(args)
        raise AssertionError("expected RekeyError for a key path that is not git-ignored")
    except ingest_module.RekeyError:
        pass


def test_rekey_output_safety_scan_trips_on_key_leakage() -> None:
    real_key = bytes([0x5A] * 16)
    leaking_frame = _make_raw_frame("tx", 0x00, _REKEY_DEVICE_ID, _REKEY_CONTROLLER_ID, 0x32, real_key)
    try:
        ingest_module.assert_no_key_leakage([leaking_frame], real_key)
        raise AssertionError("expected RekeyError when a frame still contains the real key's bytes")
    except ingest_module.RekeyError:
        pass
    # A frame that does NOT contain the key must pass cleanly.
    clean_frame = _make_raw_frame("tx", 0x00, _REKEY_DEVICE_ID, _REKEY_CONTROLLER_ID, 0x32, bytes(16))
    ingest_module.assert_no_key_leakage([clean_frame], real_key)  # must not raise


def test_validate_hard_fails_key_transfer_not_decrypting_to_corpus_key() -> None:
    """validate.py's 0x32 safety net must hard-fail regardless of the capture's `key:` field —
    this is what stops an un-re-keyed raw pairing capture from ever being committed.
    """
    wrong_key = bytes([0x5A] * 16)
    key_init = _make_raw_frame("tx", 0x40, _REKEY_DEVICE_ID, _REKEY_CONTROLLER_ID, protolib.CMD_KEY_INIT, b"")
    challenge_bytes = bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0x66])
    challenge = _make_raw_frame("rx", 0x00, _REKEY_CONTROLLER_ID, _REKEY_DEVICE_ID, protolib.CMD_CHALLENGE_REQ,
                                challenge_bytes)
    # Encrypted under wrong_key, never rekeyed to the corpus key — this must be rejected.
    encrypted = protolib.crypt_key(bytes([protolib.CMD_KEY_INIT]), challenge_bytes, wrong_key)
    transfer = _make_raw_frame("tx", 0x00, _REKEY_DEVICE_ID, _REKEY_CONTROLLER_ID, protolib.CMD_KEY_TRANSFER,
                               encrypted)

    data = {
        "key": "unknown",  # even key:unknown must be rejected — the field is not a safety boundary
        "frames": [
            {"dir": "tx", "hex": key_init.hex_bytes, "crc": "absent"},
            {"dir": "rx", "hex": challenge.hex_bytes, "crc": "absent"},
            {"dir": "tx", "hex": transfer.hex_bytes, "crc": "absent"},
        ],
    }
    try:
        validate_module.validate_crypto(data, "fake_unsafe_pairing_capture")
        raise AssertionError("expected ValidationError for a 0x32 payload not decrypting to the corpus key")
    except validate_module.ValidationError:
        pass


TESTS = [
    test_clean_io_capture_sx1262,
    test_io_frame_legacy_dedup_and_no_cmd_field,
    test_no_crc_write_private,
    test_mangled_paste_fallback_tier,
    test_merge_prefers_nonzero_freq_and_t_ms,
    test_crc_ccitt_matches_known_vector,
    test_validate_ctrl0_length_mismatch_is_rejected,
    test_validate_crc_mismatch_is_rejected,
    test_validate_unknown_expect_key_is_rejected,
    test_validate_over_length_expect_frames_is_rejected,
    test_validate_bad_classification_name_is_rejected,
    test_build_partial_flag_expectation_is_rejected,
    test_build_output_is_deterministic,
    test_end_to_end_scaffold_validate_build_roundtrip,
    test_crypto_kat_vectors_match_cpp,
    test_rekey_happy_path_rewrites_hmac_under_corpus_key,
    test_rekey_tamper_abort,
    test_rekey_refuses_non_gitignored_key_path,
    test_rekey_output_safety_scan_trips_on_key_leakage,
    test_validate_hard_fails_key_transfer_not_decrypting_to_corpus_key,
    test_io_frame_only_retry_not_merged,
]


def main() -> int:
    failures = []
    for test in TESTS:
        try:
            test()
        except AssertionError as exc:
            failures.append(f"{test.__name__}: {exc}")
        except Exception as exc:  # noqa: BLE001 - surface any unexpected error as a failure
            failures.append(f"{test.__name__}: unexpected {type(exc).__name__}: {exc}")

    if failures:
        print("run_tests.py: FAILED", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1

    print(f"run_tests.py: OK ({len(TESTS)} tests)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
