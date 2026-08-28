#!/usr/bin/env python3
"""Self-tests for scripts/check-key-material.py.

Stdlib `assert`-based, no pytest -- mirrors scripts/corpus/tests/run_tests.py's dependency-light
style. Fixtures are in-memory (path, text) pairs rather than files on disk: the scanner's unit of
work is (path, text), and inline fixtures avoid committing byte runs that this very check would
then have to be taught to tolerate.

The script under test is hyphen-named (matching the scripts/check-*.py family), so it cannot be
`import`ed directly -- it is loaded via importlib.util.spec_from_file_location instead.

Run via `python3 scripts/check_key_material_test.py`; wired into `make key-material-scan`.
Exits non-zero with a description of the first failure.
"""

import importlib.util
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

_spec = importlib.util.spec_from_file_location("check_key_material", REPO_ROOT / "scripts" / "check-key-material.py")
ckm = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ckm)

# check-key-material.py's own module-level sys.path.insert() + `import protolib` already ran
# during exec_module() above, so reuse that exact module object rather than importing it again.
protolib = ckm.protolib


def _frame_bytes(ctrl0_flags: int, dst: bytes, src: bytes, cmd: int, data: bytes) -> bytes:
    """Build non-CRC frame bytes with a correct CTRL0 length field for the given payload."""
    total_len = protolib.FRAME_MIN_SIZE + len(data)
    ctrl0 = (ctrl0_flags & 0xE0) | ((total_len - 1) & protolib.CTRL0_LENGTH_MASK)
    return bytes([ctrl0, 0x00]) + dst + src + bytes([cmd]) + data


def _with_crc(frame: bytes) -> bytes:
    crc = protolib.crc_ccitt(frame)
    return frame + bytes([crc & 0xFF, (crc >> 8) & 0xFF])


def _hex_line(data: bytes) -> str:
    return " ".join(f"{b:02X}" for b in data)


_DST = bytes.fromhex("AABBCC")
_SRC = bytes.fromhex("C0FFEE")


def _key_transfer_frame() -> bytes:
    # CMD_KEY_TRANSFER payload is exactly AES_KEY_SIZE bytes (declared_len == 25).
    payload = bytes(range(protolib.AES_KEY_SIZE))
    return _frame_bytes(0x40, _DST, _SRC, protolib.CMD_KEY_TRANSFER, payload)


def _oneway_add_controller_frame(with_mac: bool) -> bytes:
    payload = bytes(range(protolib.ONEWAY_ADD_CONTROLLER_DECLARED_PAYLOAD_SIZE))
    frame = _frame_bytes(0x40, _DST, _SRC, protolib.CMD_ONEWAY_ADD_CONTROLLER, payload)
    if with_mac:
        frame += bytes(range(protolib.HMAC_SIZE))
    return frame


# --- Test cases -----------------------------------------------------------------------------


def test_crc_known_answer_vector_independently_sourced() -> None:
    # Independently computed CRC-16/KERMIT-variant (poly 0x8408 reversed, init 0x0000) over
    # b"123456789" -- computed by hand, not derived by calling protolib.crc_ccitt() on itself, so
    # a shared bug in the CRC logic can't hide in both the checker and this vector.
    assert protolib.crc_ccitt(b"123456789") == 0x2189, f"got 0x{protolib.crc_ccitt(b'123456789'):04X}"


def test_crc_valid_key_transfer_in_markdown_fires() -> None:
    frame = _with_crc(_key_transfer_frame())
    text = f"Some doc prose.\n\nCaptured frame: {_hex_line(frame)}\n\nmore prose.\n"
    findings = ckm.scan_text(Path("docs/example.md"), text)
    assert len(findings) == 1, f"expected 1 finding, got {len(findings)}"
    assert findings[0].tier == "crc-valid"
    assert findings[0].cmd == protolib.CMD_KEY_TRANSFER


def test_flipped_crc_byte_does_not_fire() -> None:
    frame = bytearray(_with_crc(_key_transfer_frame()))
    frame[-1] ^= 0xFF  # flip one CRC byte
    text = f"Captured frame: {_hex_line(bytes(frame))}\n"
    findings = ckm.scan_text(Path("docs/example.md"), text)
    assert findings == [], f"expected no findings (CRC gate must reject), got {findings}"


def test_unwatched_opcode_with_valid_crc_does_not_fire() -> None:
    # Same shape, but cmd changed to 0x04 (STATUS response) -- a non-watched opcode. Even a
    # perfectly valid CRC must not fire: this proves opcode gating, not just the CRC gate.
    payload = bytes(range(protolib.AES_KEY_SIZE))
    frame = _with_crc(_frame_bytes(0x40, _DST, _SRC, 0x04, payload))
    text = f"Captured frame: {_hex_line(frame)}\n"
    findings = ckm.scan_text(Path("docs/example.md"), text)
    assert findings == [], f"expected no findings for an unwatched opcode, got {findings}"


def test_corpus_captures_path_excluded_by_exact_prefix() -> None:
    # A valid corpus fixture's hex embedded in a *simulated* .yaml file, at a path that merely
    # contains the word "corpus" but is NOT under tests/corpus/captures/, must still fire --
    # proves the exclusion is an exact path prefix, not a loose "contains corpus" match.
    frame = _with_crc(_key_transfer_frame())
    text = f'hex: "{_hex_line(frame)}"\n'
    findings = ckm.scan_text(Path("docs/example.yaml"), text)
    assert len(findings) == 1, "docs/example.yaml (outside tests/corpus/captures/) must fire"


def test_real_corpus_captures_path_is_excluded_by_the_scope_filter() -> None:
    # The mirror of test_corpus_captures_path_excluded_by_exact_prefix: the exclusion needs
    # testing in both directions, since only testing the "outside fires" half would pass even if
    # the prefix filter were inverted or absent. scan_text() itself has no path-aware exclusion
    # logic -- that lives in _is_corpus_captures_path(), which in_scope_files() applies before
    # scan_text() is ever called -- so this asserts the boundary at the function that owns it.
    assert ckm._is_corpus_captures_path("tests/corpus/captures/foo/bar.yaml")
    # A path that merely contains the substring "corpus" but isn't under the real prefix must
    # NOT be excluded -- proves this is an exact prefix match, not a loose "contains" check.
    assert not ckm._is_corpus_captures_path("docs/example.yaml")
    assert not ckm._is_corpus_captures_path("docs/corpus-notes.yaml")


def test_base64_variant_of_key_transfer_frame_fires() -> None:
    import base64

    frame = _with_crc(_key_transfer_frame())
    encoded = base64.b64encode(frame).decode("ascii")
    text = f"Captured frame (base64): {encoded}\n"
    findings = ckm.scan_text(Path("docs/example.md"), text)
    assert len(findings) == 1, f"expected 1 base64-sourced finding, got {findings}"
    assert findings[0].tier == "crc-valid"


def test_cpp_h_hpp_suffixed_files_are_in_scope() -> None:
    # in_scope_files() globs *.cpp/*.h/*.hpp alongside the doc/yaml patterns -- confirm each
    # source suffix is actually matched by SCAN_PATTERNS, distinct from "scan_text() found
    # nothing for some other reason".
    import fnmatch

    for path in ("tests/proto_frame_test.cpp", "components/home_io_control/redaction.h", "components/home_io_control/foo.hpp"):
        assert any(fnmatch.fnmatch(path, pattern) for pattern in ckm.SCAN_PATTERNS), (
            f"no SCAN_PATTERNS entry matches {path!r}")


def test_crc_valid_key_transfer_in_cpp_comment_fires() -> None:
    # A human pasting a real captured frame into a debugging comment is exactly the leak this
    # scope expansion targets -- prove scan_text() itself (suffix-agnostic) still detects it when
    # the surrounding syntax is C++ rather than markdown.
    frame = _with_crc(_key_transfer_frame())
    text = f"// captured while debugging: {_hex_line(frame)}\nvoid foo() {{}}\n"
    findings = ckm.scan_text(Path("components/home_io_control/foo.cpp"), text)
    assert len(findings) == 1, f"expected 1 finding in a .cpp comment, got {findings}"
    assert findings[0].tier == "crc-valid"


def test_hand_built_frame_array_literal_without_valid_crc_does_not_fire() -> None:
    # Mirrors this project's actual test style: a hand-built byte array for a watched opcode with
    # no trailing CRC bytes appended (the frame is CRC-checked separately, or the array is a
    # rejection-path fixture) must not fire -- there is no valid-CRC signal to key off of, and the
    # run doesn't satisfy the CRC-less shape tier either (declared_len doesn't match a key-wrap
    # length here).
    frame = _frame_bytes(0x40, _DST, _SRC, protolib.CMD_KEY_TRANSFER, bytes(range(21)))
    text = f"const uint8_t kFrame[] = {{{', '.join(f'0x{b:02X}' for b in frame)}}};\n"
    findings = ckm.scan_text(Path("tests/proto_frame_test.cpp"), text)
    assert findings == [], f"expected no findings for a CRC-less array literal, got {findings}"


def test_oneway_add_controller_with_mac_trailer_fires_at_35_bytes() -> None:
    frame = _with_crc(_oneway_add_controller_frame(with_mac=True))
    assert len(frame) == 29 + 6 + 2, f"unexpected total length {len(frame)}"
    text = f"Captured: {_hex_line(frame)}\n"
    findings = ckm.scan_text(Path("docs/example.md"), text)
    assert len(findings) == 1, f"expected 1 finding at the MAC-trailer length, got {findings}"
    assert findings[0].cmd == protolib.CMD_ONEWAY_ADD_CONTROLLER
    assert findings[0].candidate_len == 29 + 6


def test_non_0x30_opcode_does_not_fire_at_trailer_length() -> None:
    # Same 35-non-CRC-byte shape (29-byte declared frame + 6 junk bytes that happen to look like
    # a MAC trailer), but with CMD_KEY_TRANSFER instead of CMD_ONEWAY_ADD_CONTROLLER. The
    # +HMAC_SIZE trailer candidate length is 0x30-only, so this must not fire at that length --
    # it also must not fire at the plain declared length, since declared_len (30, from a 21-byte
    # payload) isn't the CMD_KEY_TRANSFER key-wrap length (25) and this frame carries no valid
    # CRC at either candidate length.
    payload = bytes(range(21))
    base_frame = _frame_bytes(0x40, _DST, _SRC, protolib.CMD_KEY_TRANSFER, payload)
    frame = base_frame + bytes(range(protolib.HMAC_SIZE))  # 35 non-CRC bytes, no valid CRC appended
    text = f"Captured: {_hex_line(frame)}\n"
    findings = ckm.scan_text(Path("docs/example.md"), text)
    assert findings == [], f"expected no findings for a non-0x30 opcode at the trailer length, got {findings}"


def test_frame_embedded_mid_run_fires_at_correct_line() -> None:
    frame = _with_crc(_key_transfer_frame())
    junk_before = "DE AD BE EF CA FE BA BE"
    junk_after = "12 34 56 78"
    line = f"{junk_before} {_hex_line(frame)} {junk_after}"
    prefix = "noise\nmore noise\n"
    text = prefix + line + "\n"
    findings = ckm.scan_text(Path("docs/example.md"), text)
    assert len(findings) == 1, f"expected exactly 1 finding, got {findings}"
    # The frame starts after "noise\nmore noise\n" (prefix) + junk_before + a separating space,
    # all on the third line of the fixture (1-indexed). This proves the every-offset search
    # actually runs and that the reported line is derived from the file text, not the decoded
    # buffer -- every other case sits at offset 0 on line 1 and would pass even with a
    # start == 0-only scan.
    assert findings[0].line == 3, f"expected the finding on line 3, got line {findings[0].line}"


def test_shape_tier_positive_bare_25_byte_frame_no_crc() -> None:
    # The exact shape an io_capture/io_frame log paste produces: no trailing CRC, alone on a line.
    frame = _key_transfer_frame()
    assert len(frame) == 25
    text = f"{_hex_line(frame)}\n"
    findings = ckm.scan_text(Path("docs/example.md"), text)
    assert len(findings) == 1, f"expected 1 shape-tier finding, got {findings}"
    assert findings[0].tier == "shape"
    assert findings[0].cmd == protolib.CMD_KEY_TRANSFER


def test_shape_tier_negative_trailing_junk_breaks_run_end_alignment() -> None:
    frame = _key_transfer_frame()
    text = f"{_hex_line(frame)} DE AD BE EF\n"
    findings = ckm.scan_text(Path("docs/example.md"), text)
    assert findings == [], f"expected no findings when junk breaks the run-end alignment, got {findings}"


def test_shape_tier_negative_wrong_declared_length() -> None:
    # An 18-byte CMD_KEY_TRANSFER frame (the config/*.yaml loopback shape) -- declared length 18,
    # not 25 -- must keep passing.
    payload = bytes(range(9))
    frame = _frame_bytes(0x40, _DST, _SRC, protolib.CMD_KEY_TRANSFER, payload)
    assert len(frame) == 18
    text = f"{_hex_line(frame)}\n"
    findings = ckm.scan_text(Path("docs/example.md"), text)
    assert findings == [], f"expected no findings for a non-25-byte 0x32 frame, got {findings}"


def test_shape_tier_negative_challenge_opcode_not_covered() -> None:
    # A CRC-less CMD_CHALLENGE_REQ frame of any length must never fire on the shape tier -- it is
    # 0x32/0x30-only.
    payload = bytes(range(6))
    frame = _frame_bytes(0x40, _DST, _SRC, protolib.CMD_CHALLENGE_REQ, payload)
    text = f"{_hex_line(frame)}\n"
    findings = ckm.scan_text(Path("docs/example.md"), text)
    assert findings == [], f"expected no findings for a CRC-less 0x3C frame, got {findings}"


def test_legacy_io_frame_preamble_prefix_does_not_absorb_into_run() -> None:
    # Mirrors the real legacy io_frame shape (scripts/corpus/tests/data/io_frame_legacy.txt):
    # "TX [17 bytes] freq=868950000 preamble=1024: <hex>". The "preamble=1024" segment sits
    # immediately (via ": ") before the real frame's hex run with no separator inside "1024"
    # itself. A hex-run extractor that isn't anchored against absorbing adjacent decimal digits
    # pulls a spurious leading byte ("24") from "1024" ahead of the real frame, which shifts the
    # frame off run-offset 0 and defeats the CRC-less shape tier (it requires start == 0). Must
    # fire, with the frame recognized starting at the real bytes, not one byte early.
    frame = _key_transfer_frame()
    assert len(frame) == 25
    text = f"TX [17 bytes] freq=868950000 preamble=1024: {_hex_line(frame)}\n"
    findings = ckm.scan_text(Path("docs/example.md"), text)
    assert len(findings) == 1, f"expected 1 shape-tier finding, got {findings}"
    assert findings[0].tier == "shape"
    assert findings[0].declared_len == 25


def test_multiple_frames_per_fence_do_not_merge_across_lines() -> None:
    # Three separate pasted frame lines in one markdown fence must extract as three independent
    # hex runs, not merge into a single run spanning all three lines. A separator class that
    # includes \s (which matches newline) would let the mandatory separator between runs "absorb"
    # the line breaks, turning three single-frame lines into one 3-frame-long buffer -- and the
    # shape tier's "whole run is exactly one frame" check then only matches the last of the three.
    frame = _key_transfer_frame()
    text = f"```\n{_hex_line(frame)}\n{_hex_line(frame)}\n{_hex_line(frame)}\n```\n"
    findings = ckm.scan_text(Path("docs/example.md"), text)
    assert len(findings) == 3, f"expected 3 findings (one per line), got {findings}"
    assert [f.line for f in findings] == [2, 3, 4], f"expected findings on lines 2/3/4, got {[f.line for f in findings]}"
    assert all(f.tier == "shape" for f in findings)


def test_load_local_secret_key_values_filters_to_key_named_fields() -> None:
    # Only field names containing "key" come back; short non-key values (hub_node_id) are
    # deliberately excluded -- see _load_local_secret_key_values()'s docstring for why.
    import tempfile

    original = ckm.SECRETS_FILE
    try:
        with tempfile.TemporaryDirectory() as tmpdir:
            secrets_path = Path(tmpdir) / "secrets.yaml"
            secrets_path.write_text(
                'wifi_password: "notakeyfield"\n'
                'hub_system_key: "AABBCCDDEEFF00112233445566778899"\n'
                'api_key: "shortkeyvalue"\n'
                'hub_node_id: "C0FFEE"\n'
            )
            ckm.SECRETS_FILE = secrets_path
            values = ckm._load_local_secret_key_values()
            assert values == {
                "hub_system_key": "AABBCCDDEEFF00112233445566778899",
                "api_key": "shortkeyvalue",
            }, values
    finally:
        ckm.SECRETS_FILE = original


def test_load_local_secret_key_values_returns_empty_when_file_missing() -> None:
    import tempfile

    original = ckm.SECRETS_FILE
    try:
        with tempfile.TemporaryDirectory() as tmpdir:
            ckm.SECRETS_FILE = Path(tmpdir) / "does-not-exist.yaml"
            assert ckm._load_local_secret_key_values() == {}
    finally:
        ckm.SECRETS_FILE = original


def test_find_secret_occurrences_detects_verbatim_leak() -> None:
    secret_values = {"hub_system_key": "AABBCCDDEEFF00112233445566778899"}
    text = "some prose\n///AABBCCDDEEFF00112233445566778899\nmore prose\n"
    findings = ckm._find_secret_occurrences(Path("components/home_io_control/foo.h"), text, secret_values)
    assert len(findings) == 1, findings
    assert findings[0].line == 2, findings[0].line
    assert findings[0].field_name == "hub_system_key"


def test_find_secret_occurrences_no_match_returns_empty() -> None:
    secret_values = {"hub_system_key": "AABBCCDDEEFF00112233445566778899"}
    assert ckm._find_secret_occurrences(Path("foo.md"), "nothing sensitive here\n", secret_values) == []


def test_scan_for_local_secret_leaks_is_a_noop_with_no_secret_values() -> None:
    # Empty secret_values (no config/secrets.yaml locally, e.g. in CI) must short-circuit rather
    # than walking the repo -- this also proves it doesn't require a real secrets.yaml to run.
    assert ckm.scan_for_local_secret_leaks({}) == []


TESTS = [
    test_crc_known_answer_vector_independently_sourced,
    test_crc_valid_key_transfer_in_markdown_fires,
    test_flipped_crc_byte_does_not_fire,
    test_unwatched_opcode_with_valid_crc_does_not_fire,
    test_corpus_captures_path_excluded_by_exact_prefix,
    test_real_corpus_captures_path_is_excluded_by_the_scope_filter,
    test_base64_variant_of_key_transfer_frame_fires,
    test_cpp_h_hpp_suffixed_files_are_in_scope,
    test_crc_valid_key_transfer_in_cpp_comment_fires,
    test_hand_built_frame_array_literal_without_valid_crc_does_not_fire,
    test_load_local_secret_key_values_filters_to_key_named_fields,
    test_load_local_secret_key_values_returns_empty_when_file_missing,
    test_find_secret_occurrences_detects_verbatim_leak,
    test_find_secret_occurrences_no_match_returns_empty,
    test_scan_for_local_secret_leaks_is_a_noop_with_no_secret_values,
    test_oneway_add_controller_with_mac_trailer_fires_at_35_bytes,
    test_non_0x30_opcode_does_not_fire_at_trailer_length,
    test_frame_embedded_mid_run_fires_at_correct_line,
    test_shape_tier_positive_bare_25_byte_frame_no_crc,
    test_shape_tier_negative_trailing_junk_breaks_run_end_alignment,
    test_shape_tier_negative_wrong_declared_length,
    test_shape_tier_negative_challenge_opcode_not_covered,
    test_legacy_io_frame_preamble_prefix_does_not_absorb_into_run,
    test_multiple_frames_per_fence_do_not_merge_across_lines,
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
        print("check_key_material_test.py: FAILED", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1

    print(f"check_key_material_test.py: OK ({len(TESTS)} tests)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
