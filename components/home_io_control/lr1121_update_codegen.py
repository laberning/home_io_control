## @file
## @brief Schema, validation and code generation for ``home_io_control: lr1121_firmware_update:``.
## @ingroup hioc_codegen
##
## Validates the ``source:``/``checksum_md5:`` shorthand at schema time, then in to_code()
## fetches the image (cached), verifies it with lr1121_firmware.py and renders it into a
## generated C++ header, together with the update button and, for the ``bootloader:``
## sub-block, the loader image and its arming switch.

import hashlib
import logging
import urllib.error
import urllib.request

import esphome.codegen as cg
import esphome.config_validation as cv
# Aliased: this package has its own switch.py/button.py platform submodules, so the real ESPHome
# components are always imported under names that cannot be mistaken for them. In the package's
# __init__.py an unaliased `switch`/`button` would even be overwritten: __init__.py's namespace IS
# the package object, the slot ESPHome's loader binds `esphome.components.home_io_control.switch`
# into when it imports our platform file, so whichever import ran last would silently win.
from esphome.components import button as button_component
from esphome.components import switch as switch_component
from esphome.const import (
    CONF_ID,
    CONF_INVERTED,
    CONF_NAME,
    CONF_REF,
    CONF_SOURCE,
    ENTITY_CATEGORY_CONFIG,
)
from esphome.core import CORE, ID
from esphome.helpers import write_file_if_changed

from . import lr1121_firmware
from .hub_names import (
    CONF_BUSY_PIN,
    CONF_CHECKSUM_MD5,
    CONF_LR1121_BOOTLOADER,
    CONF_LR1121_BOOTLOADER_SWITCH_ID,
    CONF_LR1121_FIRMWARE_UPDATE,
    CONF_LR1121_FIRMWARE_UPDATE_BUTTON_ID,
    CONF_RADIO_TYPE,
    CONF_TARGET_VERSION,
    IOHomeLr1121BootloaderRewriteSwitch,
    IOHomeLr1121FirmwareUpdateButton,
)

_LOGGER = logging.getLogger(__name__)


def validate_lr1121_firmware_source(value, *, expect_loader=False):
    """Validate the lr1121_firmware_update `source:` shorthand at schema time.

    Checks the shape (github://owner/repo/path[@ref]) and the image class (transceiver vs.
    loader vs. modem, by filename -- see lr1121_firmware.validate_image_class()). The network
    fetch and MD5/image-content verification happen later, in to_code(), where a failure is
    still a build-time error but one that needs the network anyway.
    @param expect_loader True for the bootloader sub-block's `source:` (must be a loader image),
           False for the ordinary transceiver `source:` (must not be one).
    """
    value = cv.string_strict(value)
    try:
        _, _, path, _ = lr1121_firmware.parse_github_source(value)
        lr1121_firmware.validate_image_class(path, expect_loader=expect_loader)
    except lr1121_firmware.Lr1121FirmwareError as err:
        raise cv.Invalid(str(err)) from err
    return value


def validate_checksum_md5(value):
    """Validate checksum_md5 as exactly 32 hex characters (MD5)."""
    value = cv.string_strict(value).lower()
    if len(value) != 32:
        raise cv.Invalid("checksum_md5 must be exactly 32 hex characters (MD5)")
    try:
        int(value, 16)
    except ValueError as err:
        raise cv.Invalid("checksum_md5 must be valid hexadecimal") from err
    return value


# The bootloader sub-block's `source:` must BE a loader image (expect_loader=True) -- the
# symmetric guard to the outer schema's default expect_loader=False (C8 in the bootloader update
# ADR 0021): a transceiver image in this slot would erase and overwrite the wrong thing
# at stage 1a.
LR1121_BOOTLOADER_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_SOURCE): lambda value: validate_lr1121_firmware_source(value, expect_loader=True),
        cv.Optional(CONF_REF): cv.string_strict,
        cv.Optional(CONF_CHECKSUM_MD5): validate_checksum_md5,
    }
)

LR1121_FIRMWARE_UPDATE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_SOURCE): validate_lr1121_firmware_source,
        cv.Optional(CONF_REF): cv.string_strict,
        cv.Optional(CONF_CHECKSUM_MD5): validate_checksum_md5,
        # target_version exists solely as an escape hatch for a mirrored/renamed image whose
        # filename carries no version — NOT as a compatibility declaration. There is deliberately
        # no `requires_bootloader:` key: a user-declared compatibility claim is the wrong shape
        # for a safety check, since the build can derive it from the filename instead.
        cv.Optional(CONF_TARGET_VERSION): cv.hex_int,
        # Presence is the build flag for the bootloader-rewrite feature, exactly as the outer
        # block's presence already is for the transceiver-update feature -- see ADR 0021.
        cv.Optional(CONF_LR1121_BOOTLOADER): LR1121_BOOTLOADER_SCHEMA,
    }
)


def _validate_lr1121_bootloader_block(config):
    """Implement the build-time compatibility rule for the bootloader: sub-block (ADR 0021).

    Classifies the *outer* source:'s target against LR1121_KNOWN_BOOTLOADER_REQUIREMENTS without
    any network access (both source: filenames are already schema-validated shapes at this point,
    so parsing them again here is free). Deliberately three-way, like the runtime compatibility
    rule: an unrecognised target warns rather than errors, so the feature doesn't rot on Semtech's
    next release (see lr1121_firmware.classify_bootloader_upgrade_class()'s doc comment).
    """
    fw_config = config[CONF_LR1121_FIRMWARE_UPDATE]
    if CONF_LR1121_BOOTLOADER not in fw_config:
        return config

    target_fw = lr1121_firmware.resolve_target_version(fw_config[CONF_SOURCE], fw_config.get(CONF_TARGET_VERSION))
    upgrade_class = lr1121_firmware.classify_bootloader_upgrade_class(target_fw)
    if upgrade_class == "hard_error":
        raise cv.Invalid(
            f"lr1121_firmware_update.bootloader: is configured, but source: targets firmware 0x{target_fw:04X}, "
            "which is known to require bootloader 0x2100 -- after the bootloader rewrite this image would be "
            "unflashable, so this configuration would arm a trap. Point source: at a firmware version requiring "
            "bootloader 0x2101 (e.g. 0x0104), or remove the bootloader: block."
        )
    if upgrade_class == "unknown":
        _LOGGER.warning(
            "lr1121_firmware_update.bootloader: is configured, but source: targets an unrecognized firmware "
            "version (0x%04X); the bootloader-rewrite path will be inert at runtime until this build's "
            "compatibility table is extended for it (see lr1121_firmware_decisions.h)",
            target_fw,
        )

    parent_id = config[CONF_ID]
    base = parent_id.id if parent_id.id else "home_io_control"
    config[CONF_LR1121_BOOTLOADER_SWITCH_ID] = ID(
        f"{base}_lr1121_bootloader_switch",
        is_declaration=True,
        type=IOHomeLr1121BootloaderRewriteSwitch,
    )
    return config


def _validate_lr1121_firmware_update(config):
    """Gate + inject the button ID for the optional lr1121_firmware_update: block.

    Only runs when the block is present. Rejects configurations that can't reach the LR1121
    bootloader at all (wrong radio_type, missing busy_pin) or that would silently invert the
    bootloader-entry level (busy_pin inverted: true — bootloader entry drives BUSY to a physical
    LOW; see radio_lr1121_firmware_updater.h). Also injects the flash button's companion ID at
    validation time — see CONF_ACCEPT_FOREIGN_PAIRING_SWITCH_ID's comment (hub_names.py) for why that
    can't wait until to_code(). The bootloader:-specific checks (C3-C5, and the companion arming
    switch's ID) live in _validate_lr1121_bootloader_block() above, called at the end of this
    function so config[CONF_ID] and the reachability checks are already settled.
    """
    if CONF_LR1121_FIRMWARE_UPDATE not in config:
        return config
    try:
        lr1121_firmware.validate_bootloader_reachability(
            radio_type=config[CONF_RADIO_TYPE],
            has_busy_pin=CONF_BUSY_PIN in config,
            busy_pin_inverted=config.get(CONF_BUSY_PIN, {}).get(CONF_INVERTED, False),
        )
    except lr1121_firmware.Lr1121FirmwareError as err:
        raise cv.Invalid(str(err)) from err

    parent_id = config[CONF_ID]
    base = parent_id.id if parent_id.id else "home_io_control"
    config[CONF_LR1121_FIRMWARE_UPDATE_BUTTON_ID] = ID(
        f"{base}_lr1121_firmware_update_button",
        is_declaration=True,
        type=IOHomeLr1121FirmwareUpdateButton,
    )
    return _validate_lr1121_bootloader_block(config)


def _cached_http_fetch(cache_dir):
    """Build a `fetch(url, expected_hash=None) -> bytes` callable for
    lr1121_firmware.fetch_and_verify(), backed by an on-disk cache so repeat and offline builds
    don't re-download the same source.

    The cache key incorporates `expected_hash` (the MD5 fetch_and_verify() already resolved from
    the `.md5` sidecar or `checksum_md5:` before calling this for the `.bin`) rather than being
    `sha256(url)` alone. With the default `ref: HEAD` the URL never changes, so a plain
    url-only key means a corrupt/truncated download poisons the cache permanently -- no config
    change can ever invalidate it, since nothing about the request changes on retry. Folding the
    expected hash in means correcting a wrong `checksum_md5:` (or a fixed upstream sidecar) misses
    the poisoned entry and forces a fresh download. The `.md5` sidecar fetch itself has no
    expected_hash to key on (chicken-and-egg -- it's what supplies one for the .bin) and is cached
    under the URL alone; a corrupted sidecar is a much smaller/rarer risk than a corrupted 64+ KB
    binary, and the cache directory below is a manual escape hatch either way.

    Data that fails its own hash check is deliberately never written to the cache (verify-before-store):
    a transient network corruption then simply retries cleanly on the next build, with no
    config change needed at all.
    """
    cache_dir.mkdir(parents=True, exist_ok=True)

    def fetch(url, expected_hash=None):
        cache_key = hashlib.sha256(f"{url}|{expected_hash or ''}".encode("utf-8")).hexdigest()
        cache_path = cache_dir / cache_key
        if cache_path.exists():
            return cache_path.read_bytes()
        try:
            with urllib.request.urlopen(url, timeout=30) as response:  # noqa: S310
                data = response.read()
        except urllib.error.HTTPError as err:
            if err.code == 404:
                raise lr1121_firmware.Lr1121FirmwareNotFoundError(url) from err
            raise lr1121_firmware.Lr1121FirmwareError(f"HTTP {err.code} fetching {url}") from err
        except urllib.error.URLError as err:
            raise lr1121_firmware.Lr1121FirmwareError(f"Failed to fetch {url}: {err}") from err
        if expected_hash is None or hashlib.md5(data).hexdigest() == expected_hash:  # noqa: S324
            cache_path.write_bytes(data)
        return data

    return fetch


def _render_lr1121_image_header(image, array_name, words_name, version_name):
    """Render a verified firmware/loader image as a C++ header.

    Each raw 4-byte chunk of the `.bin` is exactly one big-endian word as Semtech's own image
    format already lays it out, so this only has to slice and format, not transform, the bytes.
    `inline const` (not `constexpr`) for the array: it is never used in a constant expression, so
    forcing constant-evaluation of up to ~61k elements would only cost compile time; `const` at
    namespace scope still lands in `.rodata` (flash) on ESP32, not RAM. Shared by
    _render_lr1121_firmware_header() (the transceiver image) and the bootloader loader image --
    same shape, different symbol names so both headers can be included from the same translation
    unit without colliding.
    """
    words = [f"0x{int.from_bytes(image.data[i : i + 4], 'big'):08X}" for i in range(0, len(image.data), 4)]
    words_per_line = 8
    body_lines = [
        "    " + ", ".join(words[i : i + words_per_line]) + "," for i in range(0, len(words), words_per_line)
    ]
    return "\n".join(
        [
            "#pragma once",
            "// Auto-generated by the home_io_control lr1121_firmware_update build step. Do not edit.",
            "#include <cstddef>",
            "#include <cstdint>",
            "",
            "namespace esphome {",
            "namespace home_io_control {",
            "",
            f"inline const uint32_t {array_name}[] = {{",
            *body_lines,
            "};",
            f"inline constexpr size_t {words_name} = {len(words)};",
            f"inline constexpr uint16_t {version_name} = 0x{image.version:04X};",
            "",
            "}  // namespace home_io_control",
            "}  // namespace esphome",
            "",
        ]
    )


def _render_lr1121_firmware_header(image):
    """Render the verified transceiver firmware image as a C++ header."""
    return _render_lr1121_image_header(
        image, "LR1121_FIRMWARE_UPDATE_IMAGE", "LR1121_FIRMWARE_UPDATE_IMAGE_WORDS", "LR1121_FIRMWARE_UPDATE_TARGET_VERSION"
    )


def _render_lr1121_bootloader_loader_header(image):
    """Render the verified bootloader *loader* image as a C++ header (ADR 0021)."""
    return _render_lr1121_image_header(
        image, "LR1121_BOOTLOADER_LOADER_IMAGE", "LR1121_BOOTLOADER_LOADER_IMAGE_WORDS", "LR1121_BOOTLOADER_LOADER_FW"
    )


async def _create_lr1121_firmware_update(config, var):
    """Fetch/verify the configured firmware image, generate its header, set the build flag that
    gates the whole feature, and create the "Flash LR1121 Radio Firmware" button.

    The block's mere presence in YAML is the build flag (ADR 0020) — there is no
    separate enable switch, so entering/leaving flash mode is a recompile + OTA each way.
    """
    fw_config = config[CONF_LR1121_FIRMWARE_UPDATE]
    cache_dir = CORE.data_dir / "lr1121_firmware_cache"
    try:
        image = lr1121_firmware.fetch_and_verify(
            source=fw_config[CONF_SOURCE],
            ref=fw_config.get(CONF_REF),
            checksum_md5=fw_config.get(CONF_CHECKSUM_MD5),
            target_version=fw_config.get(CONF_TARGET_VERSION),
            fetch=_cached_http_fetch(cache_dir),
        )
    except lr1121_firmware.Lr1121FirmwareError as err:
        raise cv.Invalid(f"lr1121_firmware_update: {err}") from err

    header_path = CORE.relative_src_path("lr1121_firmware_update_image.h")
    write_file_if_changed(header_path, _render_lr1121_firmware_header(image))

    cg.add_define("IOHOME_LR1121_FIRMWARE_UPDATE")

    if CONF_LR1121_BOOTLOADER in fw_config:
        await _create_lr1121_bootloader_update(fw_config[CONF_LR1121_BOOTLOADER], config, var, cache_dir)

    entity_config = button_component.button_schema(
        IOHomeLr1121FirmwareUpdateButton,
        entity_category=ENTITY_CATEGORY_CONFIG,
    ).extend(cv.COMPONENT_SCHEMA)(
        {
            CONF_ID: config[CONF_LR1121_FIRMWARE_UPDATE_BUTTON_ID],
            CONF_NAME: "Flash LR1121 Radio Firmware",
        }
    )
    entity = await button_component.new_button(entity_config)
    await cg.register_component(entity, entity_config)
    cg.add(entity.set_parent(var))


async def _create_lr1121_bootloader_update(bootloader_config, config, var, cache_dir):
    """Fetch/verify the configured loader image, generate its header, set the build flag that
    gates the bootloader-rewrite feature, and create the arming switch.

    Mirrors _create_lr1121_firmware_update() above -- same "block's presence is the build flag"
    shape, one level down (ADR 0021). `target_version` is not passed to
    fetch_and_verify(): the loader is not a "target" the way the transceiver image is, its version
    is only ever compared for *equality* against the currently-running bootloader (Semtech's
    rule), so there is nothing to override.
    """
    try:
        loader_image = lr1121_firmware.fetch_and_verify(
            source=bootloader_config[CONF_SOURCE],
            ref=bootloader_config.get(CONF_REF),
            checksum_md5=bootloader_config.get(CONF_CHECKSUM_MD5),
            target_version=None,
            fetch=_cached_http_fetch(cache_dir),
        )
    except lr1121_firmware.Lr1121FirmwareError as err:
        raise cv.Invalid(f"lr1121_firmware_update.bootloader: {err}") from err

    header_path = CORE.relative_src_path("lr1121_bootloader_loader_image.h")
    write_file_if_changed(header_path, _render_lr1121_bootloader_loader_header(loader_image))

    cg.add_define("IOHOME_LR1121_BOOTLOADER_UPDATE")

    entity_config = switch_component.switch_schema(
        IOHomeLr1121BootloaderRewriteSwitch,
        default_restore_mode="ALWAYS_OFF",  # never auto-arm after a reboot -- ADR 0021
        entity_category=ENTITY_CATEGORY_CONFIG,
    ).extend(cv.COMPONENT_SCHEMA)(
        {
            CONF_ID: config[CONF_LR1121_BOOTLOADER_SWITCH_ID],
            CONF_NAME: "Allow LR1121 Bootloader Rewrite (Irreversible)",
            # Deliberately NOT disabled_by_default. It reads like the right call for an irreversible
            # control, but in Home Assistant that disables the entity in the registry: it cannot be
            # toggled until the user finds it and enables it by hand, which makes the documented
            # procedure ("turn the switch on, press the button") simply not work. It also defeats
            # ADR 0021's reason for choosing a switch over an invisible confirmation window -- that
            # the armed state is answerable by looking -- since a disabled entity is not shown at
            # all. entity_category=config is the right amount of out-of-the-way: it files the switch
            # under Configuration rather than among the primary controls, and it stays usable.
            # The real gating is elsewhere and unaffected: the bootloader: block must be in YAML and
            # the firmware rebuilt, and the switch is off on every boot (ALWAYS_OFF).
        }
    )
    entity = await switch_component.new_switch(entity_config)
    await cg.register_component(entity, entity_config)
    cg.add(entity.set_parent(var))
