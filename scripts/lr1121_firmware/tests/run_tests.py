#!/usr/bin/env python3
"""Self-tests for lr1121_firmware.py (source parsing, MD5 verification, image validation).

Stdlib `assert`-based, no pytest -- mirrors scripts/corpus/tests/run_tests.py's dependency-light
style, and for the same reason: lr1121_firmware.py deliberately avoids importing ESPHome so this
runner works in CI without ESPHome installed (see that module's file header).

Run via `python3 scripts/lr1121_firmware/tests/run_tests.py`; wired into `make lint`.
Exits non-zero with a description of the first failure.
"""

import hashlib
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "components" / "home_io_control"))

import lr1121_firmware as fw  # noqa: E402


# === parse_github_source ===============================================================


def test_parse_github_source_with_ref() -> None:
    owner, repo, path, ref = fw.parse_github_source(
        "github://Lora-net/radio_firmware_images/lr1121/transceiver/lr1121_transceiver_0103.bin@v1.2.3"
    )
    assert owner == "Lora-net"
    assert repo == "radio_firmware_images"
    assert path == "lr1121/transceiver/lr1121_transceiver_0103.bin"
    assert ref == "v1.2.3"


def test_parse_github_source_defaults_ref_to_head() -> None:
    _, _, _, ref = fw.parse_github_source("github://owner/repo/path/to/file.bin")
    assert ref == "HEAD"


def test_parse_github_source_rejects_malformed_input() -> None:
    for bad in ["not-a-github-url", "github://owner-only", "github:///missing/owner", 123]:
        try:
            fw.parse_github_source(bad)
            raise AssertionError(f"expected Lr1121FirmwareError for {bad!r}")
        except fw.Lr1121FirmwareError:
            pass


def test_resolve_raw_url() -> None:
    url = fw.resolve_raw_url("Lora-net", "radio_firmware_images", "lr1121/transceiver/x_0103.bin", "HEAD")
    assert url == "https://raw.githubusercontent.com/Lora-net/radio_firmware_images/HEAD/lr1121/transceiver/x_0103.bin"


# === parse_md5_sidecar ===============================================================


def test_parse_md5_sidecar_bare_filename_form() -> None:
    text = "a9bb3e564b40d12ffa6614fb3431b392  lr1121_transceiver_0102.bin\n"
    assert fw.parse_md5_sidecar(text) == "a9bb3e564b40d12ffa6614fb3431b392"


def test_parse_md5_sidecar_release_artefact_path_form() -> None:
    text = "935da0dbd6d7ef9037bbe74bac8c54d7  ./release_artefacts/lr1121/transceiver/lr1121_transceiver_0104.bin\n"
    assert fw.parse_md5_sidecar(text) == "935da0dbd6d7ef9037bbe74bac8c54d7"


def test_parse_md5_sidecar_rejects_wrong_length_hash() -> None:
    try:
        fw.parse_md5_sidecar("deadbeef  short.bin")
        raise AssertionError("expected Lr1121FirmwareError for a short hash")
    except fw.Lr1121FirmwareError:
        pass


# === parse_version_from_filename ===============================================================


def test_parse_version_from_filename() -> None:
    assert fw.parse_version_from_filename("lr1121/transceiver/lr1121_transceiver_0104.bin") == 0x0104
    assert fw.parse_version_from_filename("lr1121/transceiver/lr1121_transceiver_0101.bin") == 0x0101


def test_parse_version_from_filename_unparseable_returns_none() -> None:
    assert fw.parse_version_from_filename("lr1121/transceiver/my_renamed_image.bin") is None


# === validate_image ===============================================================


def test_validate_image_rejects_non_word_aligned_length() -> None:
    try:
        fw.validate_image(b"\x00" * 5, "https://raw.githubusercontent.com/x/y/HEAD/lr1121_transceiver_0104.bin")
        raise AssertionError("expected Lr1121FirmwareError for a length not a multiple of 4")
    except fw.Lr1121FirmwareError:
        pass


def test_validate_image_accepts_large_but_valid_image() -> None:
    # Mirrors 0101's 61320-word image (245280 bytes) -- ~3.7x the size of later releases, and
    # must still pass: there is deliberately no size band.
    data = b"\x00" * (61320 * 4)
    fw.validate_image(data, "https://raw.githubusercontent.com/Lora-net/x/HEAD/lr1121/transceiver/y_0101.bin")


def test_validate_image_rejects_empty_image() -> None:
    # len(data) % 4 == 0 is trivially true for a 0-byte image, so the empty case must be
    # checked separately -- otherwise a corrupt/zero-byte download generates a zero-word array,
    # and a press erases the chip and writes nothing back.
    try:
        fw.validate_image(b"", "https://raw.githubusercontent.com/x/y/HEAD/lr1121_transceiver_0104.bin")
        raise AssertionError("expected Lr1121FirmwareError for an empty (0-byte) image")
    except fw.Lr1121FirmwareError:
        pass


def test_validate_image_rejects_non_lr1121_url() -> None:
    try:
        fw.validate_image(b"\x00" * 8, "https://raw.githubusercontent.com/x/y/HEAD/sx1262_image.bin")
        raise AssertionError("expected Lr1121FirmwareError for a URL not naming lr1121")
    except fw.Lr1121FirmwareError:
        pass


# === validate_image_class ===============================================================


def test_validate_image_class_accepts_ordinary_transceiver_source() -> None:
    fw.validate_image_class("lr1121/transceiver/lr1121_transceiver_0104.bin", expect_loader=False)


def test_validate_image_class_rejects_loader_as_transceiver_source() -> None:
    try:
        fw.validate_image_class("lr1121/loader/lr1121_loader_2100.bin", expect_loader=False)
        raise AssertionError("expected Lr1121FirmwareError for a loader image used as a transceiver source")
    except fw.Lr1121FirmwareError:
        pass


def test_validate_image_class_rejects_modem_as_transceiver_source() -> None:
    try:
        fw.validate_image_class("lr1121/modem/lr1121_modem_2.0.1.bin", expect_loader=False)
        raise AssertionError("expected Lr1121FirmwareError for a modem image used as a transceiver source")
    except fw.Lr1121FirmwareError:
        pass


def test_validate_image_class_requires_loader_for_bootloader_slot() -> None:
    fw.validate_image_class("lr1121/loader/lr1121_loader_2100.bin", expect_loader=True)
    try:
        fw.validate_image_class("lr1121/transceiver/lr1121_transceiver_0104.bin", expect_loader=True)
        raise AssertionError("expected Lr1121FirmwareError for a transceiver image in the bootloader loader slot")
    except fw.Lr1121FirmwareError:
        pass


# === resolve_target_version / classify_bootloader_upgrade_class =====================================


def test_resolve_target_version_from_filename() -> None:
    assert fw.resolve_target_version("github://o/r/lr1121/transceiver/lr1121_transceiver_0104.bin", None) == 0x0104


def test_resolve_target_version_override_wins_over_filename() -> None:
    assert (
        fw.resolve_target_version("github://o/r/lr1121/transceiver/lr1121_transceiver_0103.bin", 0x0105) == 0x0105
    )


def test_resolve_target_version_unparseable_filename_is_unknown() -> None:
    assert fw.resolve_target_version("github://o/r/lr1121/transceiver/my_renamed_image.bin", None) == 0


def test_classify_bootloader_upgrade_class_c3_known_target_needing_new_bootloader_accepts() -> None:
    assert fw.classify_bootloader_upgrade_class(0x0104) == "accept"


def test_classify_bootloader_upgrade_class_c4_known_target_needing_old_bootloader_is_hard_error() -> None:
    assert fw.classify_bootloader_upgrade_class(0x0103) == "hard_error"
    assert fw.classify_bootloader_upgrade_class(0x0101) == "hard_error"


def test_classify_bootloader_upgrade_class_c5_unknown_target_is_unknown() -> None:
    assert fw.classify_bootloader_upgrade_class(0x0999) == "unknown"


# === validate_bootloader_reachability ===============================================================


def test_validate_bootloader_reachability_accepts_valid_config() -> None:
    fw.validate_bootloader_reachability(radio_type="lr1121", has_busy_pin=True, busy_pin_inverted=False)


def test_validate_bootloader_reachability_rejects_wrong_radio_type() -> None:
    try:
        fw.validate_bootloader_reachability(radio_type="sx1262", has_busy_pin=True, busy_pin_inverted=False)
        raise AssertionError("expected Lr1121FirmwareError for radio_type != lr1121")
    except fw.Lr1121FirmwareError:
        pass


def test_validate_bootloader_reachability_rejects_missing_busy_pin() -> None:
    try:
        fw.validate_bootloader_reachability(radio_type="lr1121", has_busy_pin=False, busy_pin_inverted=False)
        raise AssertionError("expected Lr1121FirmwareError for a missing busy_pin")
    except fw.Lr1121FirmwareError:
        pass


def test_validate_bootloader_reachability_rejects_inverted_busy_pin() -> None:
    # Bootloader entry drives BUSY to a physical LOW; an inverted pin would invert that level.
    try:
        fw.validate_bootloader_reachability(radio_type="lr1121", has_busy_pin=True, busy_pin_inverted=True)
        raise AssertionError("expected Lr1121FirmwareError for an inverted busy_pin")
    except fw.Lr1121FirmwareError:
        pass


# === fetch_and_verify ===============================================================


def _make_fetch(bin_data: bytes, sidecar_text: str | None):
    url_suffix = "lr1121/transceiver/lr1121_transceiver_0103.bin"

    # expected_hash is accepted (a cache-key hint for __init__.py's real fetch) and ignored --
    # this fake has no cache, so it plays no role here.
    def fetch(url: str, expected_hash: str | None = None) -> bytes:
        del expected_hash
        if url.endswith(".md5"):
            if sidecar_text is None:
                raise fw.Lr1121FirmwareNotFoundError(url)
            return sidecar_text.encode("utf-8")
        assert url.endswith(url_suffix), url
        return bin_data

    return fetch


def test_fetch_and_verify_happy_path() -> None:
    data = b"\x01\x02\x03\x04" * 16
    digest = hashlib.md5(data).hexdigest()  # noqa: S324 -- test fixture, not a security use.
    fetch = _make_fetch(data, f"{digest}  lr1121_transceiver_0103.bin\n")

    image = fw.fetch_and_verify(
        source="github://Lora-net/radio_firmware_images/lr1121/transceiver/lr1121_transceiver_0103.bin",
        ref=None,
        checksum_md5=None,
        target_version=None,
        fetch=fetch,
    )
    assert image.data == data
    assert image.version == 0x0103


def test_fetch_and_verify_checksum_mismatch_fails() -> None:
    data = b"\x01\x02\x03\x04" * 16
    wrong_digest = hashlib.md5(b"not the data").hexdigest()  # noqa: S324
    fetch = _make_fetch(data, f"{wrong_digest}  lr1121_transceiver_0103.bin\n")

    try:
        fw.fetch_and_verify(
            source="github://o/r/lr1121/transceiver/lr1121_transceiver_0103.bin",
            ref=None,
            checksum_md5=None,
            target_version=None,
            fetch=fetch,
        )
        raise AssertionError("expected Lr1121FirmwareError for an MD5 mismatch")
    except fw.Lr1121FirmwareError:
        pass


def test_fetch_and_verify_missing_sidecar_falls_back_to_checksum_md5() -> None:
    data = b"\x01\x02\x03\x04" * 16
    digest = hashlib.md5(data).hexdigest()  # noqa: S324
    fetch = _make_fetch(data, sidecar_text=None)

    image = fw.fetch_and_verify(
        source="github://o/r/lr1121/transceiver/lr1121_transceiver_0103.bin",
        ref=None,
        checksum_md5=digest,
        target_version=None,
        fetch=fetch,
    )
    assert image.data == data


def test_fetch_and_verify_missing_sidecar_and_no_checksum_fails() -> None:
    data = b"\x01\x02\x03\x04" * 16
    fetch = _make_fetch(data, sidecar_text=None)

    try:
        fw.fetch_and_verify(
            source="github://o/r/lr1121/transceiver/lr1121_transceiver_0103.bin",
            ref=None,
            checksum_md5=None,
            target_version=None,
            fetch=fetch,
        )
        raise AssertionError("expected Lr1121FirmwareError when neither a sidecar nor checksum_md5 is available")
    except fw.Lr1121FirmwareError:
        pass


def test_fetch_and_verify_sidecar_and_checksum_md5_disagree_fails() -> None:
    data = b"\x01\x02\x03\x04" * 16
    digest = hashlib.md5(data).hexdigest()  # noqa: S324
    other_digest = hashlib.md5(b"different").hexdigest()  # noqa: S324
    fetch = _make_fetch(data, f"{digest}  lr1121_transceiver_0103.bin\n")

    try:
        fw.fetch_and_verify(
            source="github://o/r/lr1121/transceiver/lr1121_transceiver_0103.bin",
            ref=None,
            checksum_md5=other_digest,
            target_version=None,
            fetch=fetch,
        )
        raise AssertionError("expected Lr1121FirmwareError when checksum_md5 disagrees with the sidecar")
    except fw.Lr1121FirmwareError:
        pass


def test_fetch_and_verify_unparseable_filename_yields_unknown_version() -> None:
    def fetch(url: str, expected_hash: str | None = None) -> bytes:
        del expected_hash
        if url.endswith(".md5"):
            raise fw.Lr1121FirmwareNotFoundError(url)
        return b"\x00\x00\x00\x00"

    digest = hashlib.md5(b"\x00\x00\x00\x00").hexdigest()  # noqa: S324
    image = fw.fetch_and_verify(
        source="github://o/r/lr1121/transceiver/my_renamed_image.bin",
        ref=None,
        checksum_md5=digest,
        target_version=None,
        fetch=fetch,
    )
    assert image.version == 0, "unparseable filename with no target_version override must be 'unknown' (0)"


def test_fetch_and_verify_target_version_overrides_filename() -> None:
    data = b"\x00\x00\x00\x00"
    digest = hashlib.md5(data).hexdigest()  # noqa: S324
    fetch = _make_fetch(data, f"{digest}  lr1121_transceiver_0103.bin\n")

    image = fw.fetch_and_verify(
        source="github://o/r/lr1121/transceiver/lr1121_transceiver_0103.bin",
        ref=None,
        checksum_md5=None,
        target_version=0x0105,
        fetch=fetch,
    )
    assert image.version == 0x0105, "explicit target_version must override the filename-derived version"


def test_fetch_and_verify_rejects_non_word_aligned_image() -> None:
    data = b"\x00\x00\x00"  # 3 bytes -- not a multiple of 4
    digest = hashlib.md5(data).hexdigest()  # noqa: S324
    fetch = _make_fetch(data, f"{digest}  lr1121_transceiver_0103.bin\n")

    try:
        fw.fetch_and_verify(
            source="github://o/r/lr1121/transceiver/lr1121_transceiver_0103.bin",
            ref=None,
            checksum_md5=None,
            target_version=None,
            fetch=fetch,
        )
        raise AssertionError("expected Lr1121FirmwareError for a non-word-aligned image")
    except fw.Lr1121FirmwareError:
        pass


def test_fetch_and_verify_rejects_empty_image() -> None:
    data = b""
    digest = hashlib.md5(data).hexdigest()  # noqa: S324
    fetch = _make_fetch(data, f"{digest}  lr1121_transceiver_0103.bin\n")

    try:
        fw.fetch_and_verify(
            source="github://o/r/lr1121/transceiver/lr1121_transceiver_0103.bin",
            ref=None,
            checksum_md5=None,
            target_version=None,
            fetch=fetch,
        )
        raise AssertionError("expected Lr1121FirmwareError for an empty image")
    except fw.Lr1121FirmwareError:
        pass


TESTS = [
    test_parse_github_source_with_ref,
    test_parse_github_source_defaults_ref_to_head,
    test_parse_github_source_rejects_malformed_input,
    test_resolve_raw_url,
    test_parse_md5_sidecar_bare_filename_form,
    test_parse_md5_sidecar_release_artefact_path_form,
    test_parse_md5_sidecar_rejects_wrong_length_hash,
    test_parse_version_from_filename,
    test_parse_version_from_filename_unparseable_returns_none,
    test_validate_image_rejects_non_word_aligned_length,
    test_validate_image_accepts_large_but_valid_image,
    test_validate_image_rejects_empty_image,
    test_validate_image_rejects_non_lr1121_url,
    test_validate_image_class_accepts_ordinary_transceiver_source,
    test_validate_image_class_rejects_loader_as_transceiver_source,
    test_validate_image_class_rejects_modem_as_transceiver_source,
    test_validate_image_class_requires_loader_for_bootloader_slot,
    test_resolve_target_version_from_filename,
    test_resolve_target_version_override_wins_over_filename,
    test_resolve_target_version_unparseable_filename_is_unknown,
    test_classify_bootloader_upgrade_class_c3_known_target_needing_new_bootloader_accepts,
    test_classify_bootloader_upgrade_class_c4_known_target_needing_old_bootloader_is_hard_error,
    test_classify_bootloader_upgrade_class_c5_unknown_target_is_unknown,
    test_validate_bootloader_reachability_accepts_valid_config,
    test_validate_bootloader_reachability_rejects_wrong_radio_type,
    test_validate_bootloader_reachability_rejects_missing_busy_pin,
    test_validate_bootloader_reachability_rejects_inverted_busy_pin,
    test_fetch_and_verify_happy_path,
    test_fetch_and_verify_checksum_mismatch_fails,
    test_fetch_and_verify_missing_sidecar_falls_back_to_checksum_md5,
    test_fetch_and_verify_missing_sidecar_and_no_checksum_fails,
    test_fetch_and_verify_sidecar_and_checksum_md5_disagree_fails,
    test_fetch_and_verify_unparseable_filename_yields_unknown_version,
    test_fetch_and_verify_target_version_overrides_filename,
    test_fetch_and_verify_rejects_non_word_aligned_image,
    test_fetch_and_verify_rejects_empty_image,
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
