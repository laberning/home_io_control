## @file
## @brief Fetch, verify, and interpret a user-referenced LR1121 transceiver firmware image.
## @ingroup hioc_codegen
##
## Pure logic with the network fetch injected as a callable, so every function here is
## testable offline (see scripts/lr1121_firmware/tests/run_tests.py, wired into `make lint`).
## Deliberately does NOT import esphome.config_validation: that test runner runs on a bare
## host Python without ESPHome installed (mirroring scripts/corpus/*.py's independence), so a
## module-level `import esphome...` here would break it. Every failure raises
## Lr1121FirmwareError; __init__.py is the only place that translates it to cv.Invalid, at the
## config-validation/codegen boundary where ESPHome is guaranteed to be present.
##
## The goal this module serves is "flash whatever LR1121 transceiver image the user points at",
## not one specific release (ADR 0020). That is why validate_image()
## deliberately does not gate on image size (published images range from 65 KB to 245 KB, all
## legitimate) and why an unparseable filename version is carried through as "unknown" rather
## than failing the build.

import hashlib
import re
from dataclasses import dataclass


class Lr1121FirmwareError(Exception):
    """A configured firmware source is malformed, unverifiable, or fails validation."""


class Lr1121FirmwareNotFoundError(Lr1121FirmwareError):
    """The requested URL does not exist (e.g. HTTP 404).

    A distinct subclass so fetch_and_verify() can tell "the optional .md5 sidecar is simply
    absent" (fall back to checksum_md5) apart from any other fetch failure (network error,
    malformed response, ...), which must not be silently swallowed.
    """


# github://<owner>/<repo>/<path/to/file.bin>[@ref] -- deliberately not ESPHome's own
# cv.SOURCE_SCHEMA shorthand, which is repo-level only (no sub-path) and so cannot name a file.
_GITHUB_SOURCE_RE = re.compile(r"^github://(?P<owner>[^/]+)/(?P<repo>[^/]+)/(?P<path>[^@]+?)(?:@(?P<ref>[^@]+))?$")

_MD5_HEX_RE = re.compile(r"^[0-9a-f]{32}$")

# `..._0104.bin` -> major=0x01, minor=0x04, matching the naming convention every image in
# Lora-net/radio_firmware_images/lr1121/transceiver uses.
_VERSION_RE = re.compile(r"_(?P<major>[0-9a-f]{2})(?P<minor>[0-9a-f]{2})\.bin$", re.IGNORECASE)


def parse_github_source(value):
    """Parse a `github://` shorthand into (owner, repo, path, ref).

    `ref` defaults to "HEAD" when no `@ref` suffix is present.
    @raises Lr1121FirmwareError if `value` doesn't match the expected shape.
    """
    if not isinstance(value, str):
        raise Lr1121FirmwareError(f"source must be a string, got {type(value).__name__}")
    match = _GITHUB_SOURCE_RE.match(value.strip())
    if match is None:
        raise Lr1121FirmwareError(
            f"Invalid source {value!r}; expected github://<owner>/<repo>/<path/to/file.bin>[@ref]"
        )
    return match.group("owner"), match.group("repo"), match.group("path"), match.group("ref") or "HEAD"


def resolve_raw_url(owner, repo, path, ref):
    """Resolve a parsed github:// source to its raw.githubusercontent.com URL."""
    return f"https://raw.githubusercontent.com/{owner}/{repo}/{ref}/{path}"


def parse_md5_sidecar(text):
    """Extract the hash from a `.bin.md5` sidecar's contents.

    Only the first whitespace-separated token is the hash. The second field is NOT a stable
    filename -- some published sidecars carry a bare filename, others a relative release-artefact
    path -- so it is read and then deliberately never validated.
    @raises Lr1121FirmwareError if the first token isn't 32 lowercase hex characters.
    """
    stripped = text.strip()
    token = stripped.split()[0] if stripped else ""
    lowered = token.lower()
    if not _MD5_HEX_RE.match(lowered):
        raise Lr1121FirmwareError(f"Malformed MD5 sidecar (expected 32 hex chars as the first token): {text!r}")
    return lowered


def validate_image_class(path, *, expect_loader):
    """Reject a `source:` naming the wrong LR1121 image class, by filename.

    A blocklist of two known-bad patterns, not an allowlist requiring e.g. "transceiver" in the
    name -- an allowlist would break a user mirroring images under their own names. Semtech's own
    tool refuses a loader image as an ordinary firmware target outright; this project had no
    such guard, and a loader/modem source previously passed validation
    and was routed through the two-press UNKNOWN_TARGET confirmation, leaving the radio running a
    non-transceiver image after the flash -- SPI still answers, but there is no radio function.
    @param expect_loader True when validating the bootloader sub-block's `source:` (must BE a
           loader image); False for the ordinary transceiver `source:` (must NOT be one).
    @raises Lr1121FirmwareError on a filename/class mismatch.
    """
    name = path.rsplit("/", 1)[-1].lower()
    is_loader = "_loader_" in name
    if expect_loader:
        if not is_loader:
            raise Lr1121FirmwareError(
                f"bootloader: source must be a bootloader loader image (filename containing '_loader_'), got {path!r}"
            )
        return
    if is_loader:
        raise Lr1121FirmwareError(
            f"source {path!r} looks like a bootloader loader image (filename containing '_loader_'); loader images "
            "are only usable through the bootloader: sub-block, not as the ordinary transceiver source:"
        )
    if "_modem_" in name:
        raise Lr1121FirmwareError(
            f"source {path!r} looks like a LoRa Basics Modem-E image (filename containing '_modem_'); modem "
            "firmware is a different product mode this component does not support"
        )


def parse_version_from_filename(path):
    """Derive a firmware version from a filename like `lr1121_transceiver_0104.bin`.

    @return The version as a 16-bit int (major<<8 | minor), or None when the filename carries
            no parseable version -- that is NOT an error, see fetch_and_verify().
    """
    match = _VERSION_RE.search(path)
    if match is None:
        return None
    return (int(match.group("major"), 16) << 8) | int(match.group("minor"), 16)


def validate_image(data, url):
    """Validate that `data` has the shape of an LR1121 transceiver firmware image.

    Deliberately narrow -- length must be a whole (and non-zero) number of 32-bit words, and the
    URL must name the chip family. No size band: published images range from 65 KB to 245 KB, both
    legitimate, so guessing a plausible size band from a sample would reject valid images.
    @raises Lr1121FirmwareError on any check failing.
    """
    if len(data) == 0:
        # `len(data) % 4 == 0` is also (trivially) true for an empty image, so this must be
        # checked separately -- otherwise a zero-byte download generates a zero-word array, and a
        # press erases the chip and writes nothing back, an unrecoverable-by-this-project state.
        raise Lr1121FirmwareError(f"Firmware image is empty (0 bytes), refusing to generate a zero-word image: {url}")
    if len(data) % 4 != 0:
        raise Lr1121FirmwareError(
            f"Firmware image length {len(data)} is not a multiple of 4 bytes (not a valid raw word image): {url}"
        )
    if "lr1121" not in url.lower():
        raise Lr1121FirmwareError(f"Resolved firmware URL does not look like an LR1121 image: {url}")


def validate_bootloader_reachability(radio_type, has_busy_pin, busy_pin_inverted):
    """Reject configurations that can't reach the LR1121 bootloader, or would reach it wrong.

    Pure mirror of __init__.py's schema-time checks -- kept here, not there, so it is
    host-testable like every other check in this module (see file header). @raises
    Lr1121FirmwareError; __init__.py is the only place that translates it to cv.Invalid.
    """
    if radio_type != "lr1121":
        raise Lr1121FirmwareError("lr1121_firmware_update requires radio_type: lr1121")
    if not has_busy_pin:
        raise Lr1121FirmwareError("lr1121_firmware_update requires busy_pin (needed to enter the chip's bootloader)")
    if busy_pin_inverted:
        raise Lr1121FirmwareError(
            "busy_pin must not be inverted when lr1121_firmware_update is configured: entering "
            "the LR1121 bootloader drives it to a physical LOW, and an inverted pin would invert "
            "that level"
        )


def resolve_target_version(source, target_version):
    """Resolve the target firmware version from `source`/`target_version` without any network access.

    Same resolution order fetch_and_verify() uses (explicit override, else filename, else 0 ==
    unknown) -- factored out so schema-time bootloader-compatibility checks (see
    classify_bootloader_upgrade_class() below) can classify a `bootloader:` block's outer target
    before to_code()'s network fetch happens.
    @raises Lr1121FirmwareError if `source` doesn't match the expected github:// shape.
    """
    _, _, path, _ = parse_github_source(source)
    return target_version or parse_version_from_filename(path) or 0


# Mirrors lr1121_firmware_decisions.h's LR1121_KNOWN_BOOTLOADER_REQUIREMENTS -- duplicated here
# because this module deliberately does not import ESPHome or link against the C++ decision
# header (see the file header), yet the bootloader:-block build-time check below must classify
# the outer target before to_code()'s network fetch happens. An advisory snapshot, not a
# compatibility authority (same discipline as the C++ table): a target absent from it is
# "unverified", never "incompatible" -- see classify_bootloader_upgrade_class(). Keep the two
# tables in sync by hand when Semtech publishes a new pairing; last checked 2026-08-07.
KNOWN_BOOTLOADER_REQUIREMENTS = {
    0x0101: 0x2100,
    0x0102: 0x2100,
    0x0103: 0x2100,
    0x0104: 0x2101,
}
LR1121_BOOTLOADER_2100 = 0x2100
LR1121_BOOTLOADER_2101 = 0x2101


def classify_bootloader_upgrade_class(target_fw):
    """Classify whether a `bootloader:` sub-block is safe given the outer `source:`'s target.

    Implements the build-time half of ADR 0021's compatibility rule: it
    must stay three-way, like the C++ side's runtime compatibility rule, or it rots the first time
    Semtech ships a new firmware release this table has never heard of.
    @return "accept" (target known, requires the new bootloader 0x2101 -- C3),
            "hard_error" (target known, requires the OLD bootloader 0x2100 -- C4; post-upgrade
            this image would be unflashable, so the block would be arming a trap),
            "unknown" (target absent from the table -- C5; accept with a warning, since refusing
            outright would rot on the next Semtech release).
    """
    required = KNOWN_BOOTLOADER_REQUIREMENTS.get(target_fw)
    if required is None:
        return "unknown"
    return "accept" if required == LR1121_BOOTLOADER_2101 else "hard_error"


@dataclass
class FirmwareImage:
    """Result of fetch_and_verify(): a verified image ready to embed in the generated header."""

    data: bytes
    version: int  ## Target firmware version (major<<8 | minor), or 0 when unknown.
    url: str  ## Resolved source URL, for logging/error messages.


def fetch_and_verify(source, ref, checksum_md5, target_version, fetch):
    """Fetch, verify, and interpret the firmware image a YAML block points at.

    Ties together every function above: resolves the URL, fetches the `.bin`, fetches its
    `.md5` sidecar (falling back to `checksum_md5` when absent, failing only if neither is
    available), verifies the hash, validates the image shape, and resolves the target version.

    @param source Raw `source:` YAML value (a github:// shorthand string).
    @param ref Optional YAML `ref:` override; takes precedence over an `@ref` embedded in `source`.
    @param checksum_md5 Optional user-supplied hash (lowercase or uppercase hex), or None.
    @param target_version Optional user-supplied version override (int), or None/0 to derive
           from the filename.
    @param fetch Callable `fetch(url: str, expected_hash: str | None = None) -> bytes`, raising
           Lr1121FirmwareNotFoundError for a definite "does not exist" (e.g. HTTP 404) and any
           other exception on a harder failure (network error, ...), which is left to propagate
           uncaught. `expected_hash`, when given, is a hint a caching `fetch` may use to key its
           cache (see __init__.py's _cached_http_fetch) -- it carries no meaning here beyond that.
    @return A verified FirmwareImage.
    @raises Lr1121FirmwareError (or whatever `fetch` raises) on any failure.
    """
    owner, repo, path, parsed_ref = parse_github_source(source)
    effective_ref = ref or parsed_ref
    url = resolve_raw_url(owner, repo, path, effective_ref)

    # The expected hash is resolved *before* fetching the .bin (not after, as an earlier revision
    # did) specifically so it can be passed into that fetch as a cache-key ingredient: with
    # the default `ref: HEAD` the URL alone never changes, so a cache keyed on the URL alone can't
    # be invalidated by fixing a wrong checksum_md5 -- a corrupt/truncated download would poison
    # every future build permanently. Keying on the hash too means a corrected expectation misses
    # the poisoned entry and re-fetches.
    sidecar_hash = None
    try:
        sidecar_text = fetch(url + ".md5").decode("utf-8")
    except Lr1121FirmwareNotFoundError:
        sidecar_text = None
    if sidecar_text is not None:
        sidecar_hash = parse_md5_sidecar(sidecar_text)

    normalized_checksum = checksum_md5.lower() if checksum_md5 else None
    if sidecar_hash is None and normalized_checksum is None:
        raise Lr1121FirmwareError(
            f"No .md5 sidecar found at {url}.md5 and no checksum_md5 was configured; cannot verify the download."
        )
    if sidecar_hash is not None and normalized_checksum is not None and sidecar_hash != normalized_checksum:
        raise Lr1121FirmwareError(
            f"checksum_md5 ({normalized_checksum}) does not match the .md5 sidecar ({sidecar_hash}) for {url}"
        )
    expected_hash = sidecar_hash or normalized_checksum

    data = fetch(url, expected_hash)

    actual_hash = hashlib.md5(data).hexdigest()  # noqa: S324 -- corruption/transit check, not tamper protection.
    if actual_hash != expected_hash:
        raise Lr1121FirmwareError(f"MD5 mismatch for {url}: expected {expected_hash}, got {actual_hash}")

    validate_image(data, url)

    version = target_version or parse_version_from_filename(path) or 0

    return FirmwareImage(data=data, version=version, url=url)
