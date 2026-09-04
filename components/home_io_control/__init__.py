## @file
## @brief ESPHome hub schema and code generation for Home IO Control.
## @ingroup hioc_codegen
##
## Defines the top-level ``home_io_control:`` YAML schema, shared validators, and the
## generated C++ hub component wiring used by the platform modules.

import hashlib
import logging
import urllib.error
import urllib.request

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome import pins
from esphome.components import spi
# Aliased: this package has its own switch.py/button.py submodules, so a plain `from
# esphome.components import switch` here would bind the name `switch` in this __init__.py's
# namespace — which, because __init__.py IS the esphome.components.home_io_control package
# object, is the exact same slot ESPHome's component loader later overwrites when it imports our
# own switch.py/button.py platform files as `esphome.components.home_io_control.switch` /
# `...button`. Without the alias, whichever import happens to run last silently wins, and
# to_code() (called even later, when tasks are flushed) can end up resolving `switch`/`button` to
# our own platform files instead of the real ESPHome components.
from esphome.components import button as button_component
from esphome.components import switch as switch_component
from esphome.components import text_sensor as text_sensor_component
from esphome.const import (
    CONF_DEVICE_ID,
    CONF_ID,
    CONF_INVERTED,
    CONF_NAME,
    CONF_PLATFORM,
    CONF_REF,
    CONF_SOURCE,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
)
from esphome.core import CORE, ID
from esphome.helpers import write_file_if_changed

from . import lr1121_firmware
from . import tuning as tuning_module

_LOGGER = logging.getLogger(__name__)

DEPENDENCIES = ["api", "spi"]
AUTO_LOAD = ["button", "climate", "cover", "light", "lock", "number", "select", "sensor", "switch", "text_sensor"]
MULTI_CONF = False

CONF_HOME_IO_CONTROL_ID = "home_io_control_id"
CONF_RST_PIN = "rst_pin"
CONF_DIO0_PIN = "dio0_pin"
CONF_DIO4_PIN = "dio4_pin"
CONF_DIO1_PIN = "dio1_pin"
CONF_BUSY_PIN = "busy_pin"
CONF_NODE_ID = "node_id"
CONF_SYSTEM_KEY = "system_key"
CONF_TX_POWER = "tx_power"
CONF_PA_PIN = "pa_pin"
CONF_RADIO_TYPE = "radio_type"
CONF_FEM_EN_PIN = "fem_en_pin"
CONF_VFEM_PIN = "vfem_pin"
CONF_FEM_PA_PIN = "fem_pa_pin"
CONF_TCXO_VOLTAGE = "tcxo_voltage"
CONF_EXPOSED_SENDERS = "exposed_senders"
CONF_ACCEPT_FOREIGN_PAIRING = "accept_foreign_pairing"
CONF_RECOVER_ONEWAY_KEY = "recover_oneway_key"
CONF_ONEWAY_CONTROLLERS = "oneway_controllers"
# Internal marker recording that an identity's node_id was derived rather than configured, so
# validation errors and the boot log can say which it was.
CONF_NODE_ID_DERIVED = "_node_id_derived"
CONF_MANUFACTURER = "manufacturer"
# Same YAML key as platform_common.py's CONF_DEVICE_TYPE, spelled here rather than imported:
# platform_common imports from this module, so the dependency cannot run the other way.
CONF_IO_DEVICE_TYPE = "io_device_type"
CONF_INITIAL_SEQUENCE = "initial_sequence"
CONF_COMMANDS = "commands"
# Per-identity 1W wire overrides (ADR 0031). execute_acei: overrides the manufacturer-derived
# ACEI byte for CMD_EXECUTE; execute_broadcast: all|typed picks the all-devices (00 00 3F) vs
# typed-class destination — a handheld-remote-vs-class-bound axis, not a vendor axis.
CONF_EXECUTE_ACEI = "execute_acei"
CONF_EXECUTE_BROADCAST = "execute_broadcast"
# The build flag for this identity's "Enroll 1W Controller" button. Presence/absence is the whole
# gate -- adding or removing this line and reflashing is the enrollment feature's entire
# lifecycle, same shape as accept_foreign_pairing/recover_oneway_key.
CONF_ENROLLMENT = "enrollment"
# Whether the "Enroll 1W Controller" button's 0x30 carries a trailing MAC. Only meaningful with
# enrollment: true -- see ONEWAY_CONTROLLER_SCHEMA's own comment and create_1w_add_controller()'s
# @warning (proto_commands.h) for why real hardware disagrees on this byte.
CONF_ENROLLMENT_WITH_MAC = "enrollment_with_mac"
# Override for the device classes a VELUX enrollment 0x30 sweep targets. Unset -> the manufacturer
# profile's list (velux: {roller_shutter, awning, dual_shutter}). Ignored by the somfy gesture.
# See resolve_oneway_wire_profile() / effective_enrollment_classes() (oneway_controller.h), ADR 0032.
CONF_ENROLLMENT_CLASSES = "enrollment_classes"
# Injected at schema time, never user-supplied: the generated buttons' IDs and the identity's
# diagnostic sensor ID (ADR 0009).
CONF_BUTTON_IDS = "button_ids"
CONF_LAST_COMMAND_SENSOR_ID = "last_command_sensor_id"
# Only present when enrollment: true (ADR 0009 -- an ID created late in to_code() is silently
# dropped at runtime).
CONF_ENROLL_BUTTON_ID = "_enroll_button_id"
CONF_DIAGNOSTIC_PROBES = "diagnostic_probes"
CONF_LR1121_FIRMWARE_UPDATE = "lr1121_firmware_update"
CONF_LR1121_BOOTLOADER = "bootloader"
CONF_CHECKSUM_MD5 = "checksum_md5"
CONF_TARGET_VERSION = "target_version"
MIN_STATUS_POLL_INTERVAL_MS = 500

# Internal config key for the "Accept Foreign Pairing" companion switch ID (injected by
# post-validator, same pattern as tuning.py's companion entity IDs — ESPHome 2026.x sizes the
# runtime component vector from IDs known at the end of schema validation, so a companion
# entity created only in to_code() would silently drop; see tuning.py::_inject_tuning_companion_ids
# for the fuller rationale).
CONF_ACCEPT_FOREIGN_PAIRING_SWITCH_ID = "_accept_foreign_pairing_switch_id"
# Internal config key for the "Recover 1W Controller Key" companion switch ID (injected by
# post-validator; same rationale as CONF_ACCEPT_FOREIGN_PAIRING_SWITCH_ID above).
CONF_RECOVER_ONEWAY_KEY_SWITCH_ID = "_recover_oneway_key_switch_id"
# Internal config key for the "Flash LR1121 Radio Firmware" companion button ID (injected by
# post-validator; same rationale as CONF_ACCEPT_FOREIGN_PAIRING_SWITCH_ID above).
CONF_LR1121_FIRMWARE_UPDATE_BUTTON_ID = "_lr1121_firmware_update_button_id"
# Internal config key for the "Allow LR1121 Bootloader Rewrite (Irreversible)" companion switch ID
# (injected by post-validator; same rationale as CONF_ACCEPT_FOREIGN_PAIRING_SWITCH_ID above --
# only present when lr1121_firmware_update.bootloader: is configured).
CONF_LR1121_BOOTLOADER_SWITCH_ID = "_lr1121_bootloader_switch_id"

home_io_control_ns = cg.esphome_ns.namespace("home_io_control")
IOHomeControlComponent = home_io_control_ns.class_(
    "IOHomeControlComponent", cg.Component, spi.SPIDevice
)
# Hub-level "Recover System Key" switch (key extraction, key_extraction_responder.cpp /
# platform_hub_controls.h). Deliberately NOT exposed via a `switch:` platform entry: earlier
# revisions dispatched on the presence/absence of `io_device_id` within switch.py, which meant an
# ordinary device-bound switch missing `io_device_id` by mistake would silently become this
# security-sensitive switch instead of failing validation. Gating it behind this boolean (created
# dynamically, like the `tuning:` UI controls) makes that class of mistake structurally
# impossible: there is no shared schema for the two to be confused under.
IOHomeAcceptForeignPairingSwitch = home_io_control_ns.class_(
    "IOHomeAcceptForeignPairingSwitch", switch_component.Switch, cg.Component
)
# Hub-level "Recover 1W Controller Key" switch (key adoption, oneway_key_adoption.cpp /
# platform_hub_controls.h). Same dynamically-created, hub-bound shape and rationale as the switch
# above. Independent of accept_foreign_pairing — the two arm different listeners (2W pairing
# responder vs. 1W add-controller broadcast).
IOHomeRecoverOneWayKeySwitch = home_io_control_ns.class_(
    "IOHomeRecoverOneWayKeySwitch", switch_component.Switch, cg.Component
)
# Hub-level "Flash LR1121 Radio Firmware" button (lr1121_firmware_update_controller.cpp /
# platform_lr1121_controls.h). Same "created dynamically from a home_io_control: sub-block, not a
# device-bound platform entry" shape as the switch above — there is no `io_device_id` to bind this
# to, it targets the hub's own radio.
IOHomeLr1121FirmwareUpdateButton = home_io_control_ns.class_(
    "IOHomeLr1121FirmwareUpdateButton", button_component.Button, cg.Component
)
# Generated 1W command buttons and their per-identity diagnostic sensor
# (platform_oneway_entities.h). Created from the `oneway_controllers:` block, never a `button:`
# entry, for the same reason as the switch above.
IOHomeOneWayCommandButton = home_io_control_ns.class_(
    "IOHomeOneWayCommandButton", button_component.Button, cg.Component
)
IOHomeOneWayLastCommandTextSensor = home_io_control_ns.class_(
    "IOHomeOneWayLastCommandTextSensor", text_sensor_component.TextSensor, cg.Component
)
# Generated 1W enrollment button (platform_oneway_entities.h), one per identity with
# `enrollment: true`. Same "created from the hub block, never a `button:` entry" reasoning as
# IOHomeOneWayCommandButton above -- see that class's comment.
IOHomeOneWayEnrollButton = home_io_control_ns.class_(
    "IOHomeOneWayEnrollButton", button_component.Button, cg.Component
)
OneWayButtonAction = home_io_control_ns.enum("OneWayButtonAction", is_class=True)
# Command names a user may list, mapped to the C++ enum. OPEN and CLOSE are positions on the
# wire, not distinct opcodes -- encode_oneway_action() (oneway_controller.h) is where that
# resolves, so this table stays a plain name->enum mapping.
ONEWAY_COMMANDS = {
    "open": OneWayButtonAction.OPEN,
    "close": OneWayButtonAction.CLOSE,
    "stop": OneWayButtonAction.STOP,
    "vent": OneWayButtonAction.VENT,
    "favorite": OneWayButtonAction.FAVORITE,
}
# Hub-level "Allow LR1121 Bootloader Rewrite (Irreversible)" arming switch
# (lr1121_firmware_update_controller.cpp / platform_lr1121_controls.h). Same dynamically-created,
# hub-bound shape as the two entities above; created only when lr1121_firmware_update.bootloader:
# is configured (see _create_lr1121_bootloader_update()).
IOHomeLr1121BootloaderRewriteSwitch = home_io_control_ns.class_(
    "IOHomeLr1121BootloaderRewriteSwitch", switch_component.Switch, cg.Component
)


def _inject_accept_foreign_pairing_switch_id(config):
    if not config[CONF_ACCEPT_FOREIGN_PAIRING]:
        return config
    parent_id = config[CONF_ID]
    base = parent_id.id if parent_id.id else "home_io_control"
    config[CONF_ACCEPT_FOREIGN_PAIRING_SWITCH_ID] = ID(
        f"{base}_accept_foreign_pairing_switch",
        is_declaration=True,
        type=IOHomeAcceptForeignPairingSwitch,
    )
    return config


def _inject_recover_oneway_key_switch_id(config):
    if not config[CONF_RECOVER_ONEWAY_KEY]:
        return config
    parent_id = config[CONF_ID]
    base = parent_id.id if parent_id.id else "home_io_control"
    config[CONF_RECOVER_ONEWAY_KEY_SWITCH_ID] = ID(
        f"{base}_recover_oneway_key_switch",
        is_declaration=True,
        type=IOHomeRecoverOneWayKeySwitch,
    )
    return config


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
    validation time — see CONF_ACCEPT_FOREIGN_PAIRING_SWITCH_ID's comment above for why that
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


PA_PIN_OPTIONS = {
    "BOOST": 0x80,
    "RFO": 0x00,
}

RADIO_TYPE_OPTIONS = {
    "sx1276": "sx1276",
    "sx1262": "sx1262",
    "lr1121": "lr1121",
}

TCXO_VOLTAGE_OPTIONS = {
    "1_6V": 0x01,
    "1_7V": 0x02,
    "1_8V": 0x03,
    "2_2V": 0x04,
    "2_4V": 0x05,
    "2_7V": 0x06,
    "3_0V": 0x07,
    "3_3V": 0x08,
}

DEVICE_TYPE_OPTIONS = {
    "unknown": 0x00,
    "venetian_blind": 0x01,
    "roller_shutter": 0x02,
    "awning": 0x03,
    "window_opener": 0x04,
    "garage_opener": 0x05,
    "light": 0x06,
    "gate_opener": 0x07,
    "rolling_door_opener": 0x08,
    "lock": 0x09,
    "blind": 0x0A,
    "screen": 0x0B,
    "dual_shutter": 0x0D,
    "heating_temperature_interface": 0x0E,
    "on_off_switch": 0x0F,
    "horizontal_awning": 0x10,
    "external_venetian_blind": 0x11,
    "louvre_blind": 0x12,
    "curtain_track": 0x13,
    "intrusion_alarm": 0x17,
    "swinging_shutter": 0x18,
}


# Mirrors proto_constants.h's MANUFACTURER_* constants (IO-Homecontrol alliance-assigned IDs) --
# lowercased versions of those constant names, so a name typo'd here is easy to spot against the
# C++ source. Not every possible byte has a name (MANUFACTURER_ID_MAX=12); an identity whose real
# manufacturer isn't in this table still works via the raw-integer escape hatch every caller of
# _resolve_named_or_raw_token() below shares.
MANUFACTURER_OPTIONS = {
    "velux": 0x01,
    "somfy": 0x02,
    "honeywell": 0x03,
    "hormann": 0x04,
    "assa_abloy": 0x05,
    "niko": 0x06,
    "window_master": 0x07,
    "renson": 0x08,
    "ciat": 0x09,
    "secuyou": 0x0A,
    "overkiz": 0x0B,
    "atlantic_group": 0x0C,
}

# Manufacturer bytes for which oneway_controller.h's resolve_oneway_wire_profile() has a real 1W
# wire profile. Keep this set in sync with that C++ switch — two values, and there is no automated
# check (scripts/check-yaml-emitters.py compares key names, not table contents).
_ONEWAY_WIRE_PROFILE_MANUFACTURERS = {
    MANUFACTURER_OPTIONS["somfy"],
    MANUFACTURER_OPTIONS["velux"],
}


def _resolve_named_or_raw_token(token, options, max_value=0xFF):
    """Resolve a lowercase, stripped token (a name from `options`, or a raw int/hex string) to
    an integer 0-`max_value`.

    Shared "named value, else raw integer" acceptance rule: validate_device_type()/
    validate_linked_remote_entry() (DEVICE_TYPE_OPTIONS) and validate_manufacturer()
    (MANUFACTURER_OPTIONS) are the same shape of small, protocol-defined enum with an escape
    hatch for values this project hasn't named yet, so the lookup lives here once.
    @raises ValueError if token is neither a known name nor a parseable integer.
    @raises cv.Invalid if token parses as an integer but is out of range.
    """
    if token in options:
        return options[token]
    return cv.int_range(min=0, max=max_value)(int(token, 0))


def _resolve_device_type_token(token):
    """Resolve a lowercase, stripped device-type token (name or raw int/hex string) to 0-255.

    Single source of truth for the "named value from DEVICE_TYPE_OPTIONS, else raw integer"
    acceptance rule shared by validate_device_type() (io_device_type) and
    validate_linked_remote_entry() (the class:<device_type> linked-remotes form) — both accept
    the exact same set of device-type spellings, so the lookup lives here once.
    @raises ValueError if token is neither a known name nor a parseable integer.
    @raises cv.Invalid if token parses as an integer but is out of range 0-255.
    """
    return _resolve_named_or_raw_token(token, DEVICE_TYPE_OPTIONS)


def validate_device_type(value):
    """Validate io_device_type as a named string or integer 0-255."""
    if isinstance(value, int):
        return cv.int_range(min=0, max=0xFF)(value)

    if isinstance(value, str):
        normalized = cv.string_strict(value).strip().lower()
        try:
            return _resolve_device_type_token(normalized)
        except ValueError as err:
            raise cv.Invalid(
                "Device type must be a known name or an integer in the range 0..255 (for example 0x11)"
            ) from err

    raise cv.Invalid(
        "Device type must be a known name or an integer in the range 0..255"
    )


def validate_manufacturer(value):
    """Validate manufacturer as a named string (MANUFACTURER_OPTIONS) or integer 0-255.

    Mirrors validate_device_type() exactly (same "name, else raw integer" shape via
    _resolve_named_or_raw_token()) — a manufacturer ID is the same kind of small,
    protocol-defined enum, it just has no linked_remotes-style second caller.
    """
    if isinstance(value, int):
        return cv.int_range(min=0, max=0xFF)(value)

    if isinstance(value, str):
        normalized = cv.string_strict(value).strip().lower()
        try:
            return _resolve_named_or_raw_token(normalized, MANUFACTURER_OPTIONS)
        except ValueError as err:
            raise cv.Invalid(
                "manufacturer must be a known name or an integer in the range 0..255 (for example 0x02)"
            ) from err

    raise cv.Invalid("manufacturer must be a known name or an integer in the range 0..255")


def device_type_expression(value):
    """Generate a C++ static_cast expression for a validated device type."""
    return cg.RawExpression(
        f"static_cast<esphome::home_io_control::DeviceType>(0x{value:02X})"
    )


def validate_node_id(value):
    """Validate node_id as exactly 6 hex characters (3 bytes)."""
    value = cv.string_strict(value).upper()
    if len(value) != 6:
        raise cv.Invalid("Node ID must be exactly 6 hex characters (3 bytes)")
    try:
        int(value, 16)
    except ValueError:
        raise cv.Invalid("Node ID must be valid hexadecimal")
    return value


def validate_system_key(value):
    """Validate system_key as exactly 32 hex characters (16 bytes)."""
    value = cv.string_strict(value).upper()
    if len(value) != 32:
        raise cv.Invalid("System key must be exactly 32 hex characters (16 bytes)")
    try:
        int(value, 16)
    except ValueError:
        raise cv.Invalid("System key must be valid hexadecimal")
    return value


# A 1W controller identity. 1W is class-addressed — a command goes to a device *class*, never to
# a node — so there is no `io_device_id` here and none on the entities that will reference this
# handle. What distinguishes one 1W control surface from another is the controller doing the
# transmitting: its address, its key, and the class it speaks to. See ADR 0027 and
# oneway_controller.h.
#
# A newly-required key here also needs a matching field in the `oneway_controllers:` block
# build_oneway_adoption_report() (hub_internal.h) hand-emits for paste-and-reflash (ADR 0018).
# `make yaml-emitter-sync` (scripts/check-yaml-emitters.py) catches drift between the two
# statically.
def _no_duplicate_enrollment_classes(value):
    """Reject a repeated class in `enrollment_classes:` -- each just retransmits the same 0x30."""
    seen = set()
    for entry in value:
        if entry in seen:
            name = next(
                (n for n, v in DEVICE_TYPE_OPTIONS.items() if v == entry), hex(entry)
            )
            raise cv.Invalid(
                f"enrollment_classes has '{name}' more than once; each entry adds a burst to the "
                "enrollment gesture, so a repeat only wastes ~1 second of it"
            )
        seen.add(entry)
    return value


ONEWAY_CONTROLLER_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.string_strict,
        # Optional and derived when omitted — see derive_oneway_node_id() for why asking the user
        # for one is an unanswerable question.
        cv.Optional(CONF_NODE_ID): validate_node_id,
        # Optional and inherits the hub's key when omitted. cv.sensitive matches the hub's own
        # system_key handling (see the main schema below) so the value is redacted from ESPHome's
        # config dump; without it a per-identity key would leak into logs verbatim.
        cv.Optional(CONF_SYSTEM_KEY): cv.sensitive(validate_system_key),
        # No default here -- see _validate_oneway_controllers() for why: it must become required,
        # not silently 0, whenever enrollment: true actually puts this byte on air. Named or raw
        # hex, same "known name, else escape hatch" shape as io_device_type below.
        cv.Optional(CONF_MANUFACTURER): validate_manufacturer,
        cv.Required(CONF_IO_DEVICE_TYPE): validate_device_type,
        # Seeds this identity's rolling counter on first use. This is the escape hatch for a
        # device that has stopped accepting commands because its stored counter ran ahead of
        # ours: bump this and reflash. Devices accept a forward jump only within a window (~1000
        # in the one documented receiver implementation), so a value that is too far ahead fails
        # exactly like one that is too far behind, and just as silently — move it in small steps.
        cv.Optional(CONF_INITIAL_SEQUENCE, default=0): cv.int_range(min=0, max=0xFFFF),
        # Which command buttons to generate. Validated against the known set rather than taken
        # as free text: a typo would otherwise produce a silently missing button, and 1W gives no
        # runtime signal that would ever reveal one.
        cv.Optional(CONF_COMMANDS, default=[]): cv.ensure_list(
            cv.one_of(*ONEWAY_COMMANDS, lower=True)
        ),
        # The build flag for this identity's "Enroll 1W Controller" button. See CONF_ENROLLMENT's
        # own comment for the lifecycle this presence/absence gates.
        cv.Optional(CONF_ENROLLMENT, default=False): cv.boolean,
        # Whether the enroll button's 0x30 carries a 6-byte MAC trailer. Real hardware disagrees:
        # most captures this project holds carry no MAC at all (default here, matching real Somfy
        # traffic), but a real Izymo has separately been shown to accept the MAC-bearing form too
        # (the published documentation vector's own shape) -- see create_1w_add_controller()'s
        # @warning (proto_commands.h). Untested manufacturers (e.g. Velux) may require one shape
        # or the other; this exists so trying the other one needs a YAML edit, not a code change.
        cv.Optional(CONF_ENROLLMENT_WITH_MAC, default=False): cv.boolean,
        # The device classes a VELUX enrollment 0x30 sweep targets. Unset -> the manufacturer
        # profile default ({roller_shutter, awning, dual_shutter} for velux). Set it to narrow the
        # sweep, e.g. [awning], once you know which class your actuator listens on. Ignored by the
        # somfy gesture (which always uses io_device_type). Max 3 -- the C++ side is a fixed array.
        # `unknown` (0x00) is rejected: it is the C++ "not overridden" sentinel
        # (effective_enrollment_classes()), so [unknown] would silently expand back to the full
        # three-class sweep -- the opposite of narrowing it. Duplicates are rejected too: each just
        # retransmits the same frame and costs ~1s of the blocking gesture for nothing.
        cv.Optional(CONF_ENROLLMENT_CLASSES): cv.All(
            cv.ensure_list(cv.All(validate_device_type, cv.int_range(min=1, max=0xFF))),
            cv.Length(min=1, max=3),
            _no_duplicate_enrollment_classes,
        ),
        # Override the manufacturer-derived ACEI byte for this identity's 1W CMD_EXECUTE frames.
        # min=1 on purpose: 0 is the C++ "not overridden" sentinel (OneWayControllerIdentity::
        # execute_acei), so accepting execute_acei: 0 would be a silent no-op instead of an error.
        # cv.hex_int allows 0x61-style spelling; the range check runs after.
        cv.Optional(CONF_EXECUTE_ACEI): cv.All(cv.hex_int, cv.int_range(min=1, max=0xFF)),
        # "all" -> address the all-devices broadcast 00 00 3F for CMD_EXECUTE (what a handheld
        # cover remote of either vendor does); "typed" (default) -> the per-class address from
        # io_device_type (current behaviour). Not a vendor axis -- see ADR 0031.
        cv.Optional(CONF_EXECUTE_BROADCAST, default="typed"): cv.one_of(
            "typed", "all", lower=True
        ),
    }
)


def derive_oneway_node_id(hub_node_id, identity_id):
    """Derive a stable 3-byte 1W source address from the hub's node_id and an identity handle.

    `node_id` is optional on a `oneway_controllers:` entry because asking a user to invent a
    3-byte radio address is an unanswerable question — nothing tells them which addresses are
    safe, and colliding with a real remote in range silently desyncs both transmitters' rolling
    sequence counters. Deriving one removes the decision while leaving an explicit value
    available to anyone who needs it.

    The derivation is done here, at schema time, rather than at runtime on the device, so that a
    derived address participates in the same collision checks as a configured one and a clash
    fails the build instead of surfacing as a device that silently ignores commands. It is a
    pure function of (hub node_id, identity id), so it is stable across builds and reproducible
    from the YAML alone.

    Uses BLAKE2b rather than Python's hash(), which is salted per-process and would produce a
    different address on every compile.
    """
    digest = hashlib.blake2b(
        f"{hub_node_id}:{identity_id}".encode(), digest_size=3
    ).digest()
    return f"{digest[0]:02X}{digest[1]:02X}{digest[2]:02X}"


def _hex_byte_array(hex_string):
    """Render a hex string as a C++ brace-initialiser list of bytes."""
    values = ", ".join(
        f"0x{hex_string[i : i + 2]}" for i in range(0, len(hex_string), 2)
    )
    return f"{{{values}}}"


def _enrollment_classes_initialiser(identity):
    """Render `enrollment_classes:` as a 3-element std::array<DeviceType,3> brace-initialiser.

    Unset, or fewer than 3, is padded with UNKNOWN (0x00) -- the sentinel effective_enrollment_classes()
    (oneway_controller.h) reads as "not overridden here" / "skip this slot".
    """
    padded = (list(identity.get(CONF_ENROLLMENT_CLASSES, [])) + [0, 0, 0])[:3]
    entries = ", ".join(
        f"static_cast<esphome::home_io_control::DeviceType>(0x{value:02X})"
        for value in padded
    )
    return f"{{{entries}}}"


def oneway_controller_expression(identity, hub_node_id):
    """Generate the C++ OneWayControllerIdentity initialiser for one configured identity.

    Emitted as a designated initialiser so the generated code reads like the YAML that produced
    it, and so adding a field to the struct cannot silently shift an existing value.
    """
    derived = identity.get(CONF_NODE_ID_DERIVED, False)
    fields = ", ".join(
        [
            f'.id = "{identity[CONF_ID]}"',
            f".node_id = {_hex_byte_array(identity[CONF_NODE_ID])}",
            f".system_key = {_hex_byte_array(identity[CONF_SYSTEM_KEY])}",
            f".manufacturer = 0x{identity[CONF_MANUFACTURER]:02X}",
            f".io_device_type = static_cast<esphome::home_io_control::DeviceType>"
            f"(0x{identity[CONF_IO_DEVICE_TYPE]:02X})",
            f".initial_sequence = 0x{identity[CONF_INITIAL_SEQUENCE]:04X}",
            f".node_id_derived = {'true' if derived else 'false'}",
            f".enrollment_with_mac = {'true' if identity[CONF_ENROLLMENT_WITH_MAC] else 'false'}",
            f".execute_acei = 0x{identity.get(CONF_EXECUTE_ACEI, 0):02X}",
            f".execute_broadcast_all = "
            f"{'true' if identity[CONF_EXECUTE_BROADCAST] == 'all' else 'false'}",
            f".enrollment_classes = {_enrollment_classes_initialiser(identity)}",
        ]
    )
    if derived:
        _LOGGER.info(
            "home_io_control: oneway_controllers '%s' node_id derived from hub %s -> %s",
            identity[CONF_ID],
            hub_node_id,
            identity[CONF_NODE_ID],
        )
    return cg.RawExpression(
        f"esphome::home_io_control::OneWayControllerIdentity{{{fields}}}"
    )


def _reject_node_id_collision(identity_id, node_id, seen_node_ids, derived):
    """Raise if `node_id` is already claimed in `seen_node_ids`; no-op otherwise.

    Shared between _validate_oneway_controllers() (collisions against the hub's own node_id and
    other oneway_controllers entries) and _final_validate_oneway_controller_addresses()
    (collisions against `linked_remotes:`/`io_device_id:` declared elsewhere in the same YAML),
    so both raise identically-worded errors regardless of which side of the config the other
    claimant lives on.
    """
    if node_id not in seen_node_ids:
        return
    owner = seen_node_ids[node_id]
    hint = (
        " (this address was derived; set node_id: explicitly to resolve the clash)"
        if derived
        else ""
    )
    raise cv.Invalid(
        f"oneway_controllers id '{identity_id}' uses node_id {node_id}, which collides with "
        f"{owner}{hint}. Two transmitters sharing an address share a rolling sequence "
        f"counter, which silently desyncs both."
    )


def _validate_oneway_controllers(config):
    """Resolve per-identity defaults and reject address/handle collisions at compile time.

    Runs as a post-validator on the whole hub config because every rule here needs the hub's own
    `node_id`/`system_key`, which a per-entry validator cannot see.

    Only checks addresses visible within `home_io_control:` itself (its own `node_id` and every
    configured `oneway_controllers` entry) — see _final_validate_oneway_controller_addresses()
    below for the matching check against `linked_remotes:`/`io_device_id:` declared elsewhere in
    the same YAML, which needs the full cross-component config and so cannot run here. Even
    together the two cannot see a real remote the user has never mentioned to this config at all;
    see derive_oneway_node_id()'s own docstring for why that residual risk cannot be validated
    away.
    """
    identities = config.get(CONF_ONEWAY_CONTROLLERS, [])
    if not identities:
        return config

    hub_node_id = config[CONF_NODE_ID]
    seen_ids = set()
    # Maps resolved address -> the human-readable owner, so a collision message can name both
    # sides rather than just reporting that "an" address is taken.
    seen_node_ids = {hub_node_id: "the hub's own node_id"}

    for identity in identities:
        identity_id = identity[CONF_ID]
        if identity_id in seen_ids:
            raise cv.Invalid(
                f"Duplicate oneway_controllers id '{identity_id}' — each identity needs its own handle"
            )
        seen_ids.add(identity_id)

        if CONF_NODE_ID not in identity:
            identity[CONF_NODE_ID] = derive_oneway_node_id(hub_node_id, identity_id)
            identity[CONF_NODE_ID_DERIVED] = True

        node_id = identity[CONF_NODE_ID]
        _reject_node_id_collision(identity_id, node_id, seen_node_ids, identity.get(CONF_NODE_ID_DERIVED))
        seen_node_ids[node_id] = f"oneway_controllers id '{identity_id}'"

        # A per-identity key is optional so that identities on the hub's own network need not
        # repeat it; an adopted foreign network's key is what makes the override necessary.
        if CONF_SYSTEM_KEY not in identity:
            identity[CONF_SYSTEM_KEY] = config[CONF_SYSTEM_KEY]

        # manufacturer becomes required, not silently 0, whenever enrollment: true actually puts
        # this byte on the air in a CMD_ONEWAY_ADD_CONTROLLER frame -- 1W gives no error a wrong
        # value would ever surface as, so the mistake has to be caught here instead. Everywhere
        # else it is genuinely unused today, so defaulting to 0 there is harmless.
        if identity[CONF_ENROLLMENT] and CONF_MANUFACTURER not in identity:
            raise cv.Invalid(
                f"oneway_controllers id '{identity_id}' has enrollment: true but no manufacturer: "
                "set. Find the value from a key-adoption report for this network (the 'Recover "
                "1W Controller Key' switch prints it), or from the device's own documentation."
            )
        # Record whether the user set manufacturer: explicitly, before it is defaulted to 0. The
        # 1W wire-profile warning below only fires for an *explicit* unprofiled vendor -- an
        # omitted manufacturer is the common back-compat case and maps silently to Somfy.
        manufacturer_explicit = CONF_MANUFACTURER in identity
        if CONF_MANUFACTURER not in identity:
            identity[CONF_MANUFACTURER] = 0

        # manufacturer: now also drives the 1W CMD_EXECUTE ACEI byte (ADR 0031). We only have a
        # verified wire profile for Somfy and Velux; any other explicitly-named vendor falls back
        # to the Somfy-shaped default, which is a guess. Warn -- but not with cv.Invalid, since an
        # unprofiled vendor is a legal config that still transmits -- and only for an identity that
        # can actually emit an EXECUTE (has commands: or enrollment:); an inert identity's wire
        # shape does not matter yet.
        identity_can_transmit = bool(identity[CONF_COMMANDS]) or identity[CONF_ENROLLMENT]
        if (
            manufacturer_explicit
            and identity[CONF_MANUFACTURER] not in _ONEWAY_WIRE_PROFILE_MANUFACTURERS
            and identity_can_transmit
        ):
            _LOGGER.warning(
                "home_io_control: oneway_controllers '%s' has manufacturer 0x%02X, which has no "
                "1W wire profile -- using the Somfy-shaped defaults (ACEI 0x43). Set execute_acei: "
                "explicitly if that is wrong for your device.",
                identity_id,
                identity[CONF_MANUFACTURER],
            )

        # A VELUX enrollment 0x30 sweep goes to {roller_shutter, awning, dual_shutter} and never
        # screen/blind/venetian_blind -- no VELUX remote pairs on those classes (issue #74 capture).
        # If this identity is a screen/blind that
        # will enroll and hasn't narrowed the sweep itself, say so: enrollment ignores io_device_type.
        if (
            identity[CONF_MANUFACTURER] == MANUFACTURER_OPTIONS["velux"]
            and identity[CONF_ENROLLMENT]
            and CONF_ENROLLMENT_CLASSES not in identity
            and identity[CONF_IO_DEVICE_TYPE]
            in (
                DEVICE_TYPE_OPTIONS["screen"],
                DEVICE_TYPE_OPTIONS["blind"],
                DEVICE_TYPE_OPTIONS["venetian_blind"],
            )
        ):
            _LOGGER.warning(
                "home_io_control: oneway_controllers '%s' is a VELUX %s with enrollment: true. The "
                "enrollment 0x30 sweep will target roller_shutter/awning/dual_shutter, NOT this "
                "io_device_type -- no VELUX remote pairs on screen/blind. Set enrollment_classes: "
                "explicitly (e.g. [awning]) if you know which class your actuator listens on.",
                identity_id,
                next(
                    name
                    for name, value in DEVICE_TYPE_OPTIONS.items()
                    if value == identity[CONF_IO_DEVICE_TYPE]
                ),
            )

        # enrollment_classes: only feeds the VELUX enrollment gesture. On a somfy/unprofiled
        # identity, or one with no enroll button at all, it is silently inert -- flag it, the same
        # way the screen/blind case above is flagged.
        if CONF_ENROLLMENT_CLASSES in identity and (
            identity[CONF_MANUFACTURER] != MANUFACTURER_OPTIONS["velux"]
            or not identity[CONF_ENROLLMENT]
        ):
            _LOGGER.warning(
                "home_io_control: oneway_controllers '%s' sets enrollment_classes: but it only "
                "affects the VELUX enrollment gesture (needs manufacturer: velux AND "
                "enrollment: true) -- it is ignored here.",
                identity_id,
            )

        # Entity IDs are declared here, at validation time, not in to_code(): an ID created late
        # is silently dropped at runtime (ADR 0009). The `<identity_id>_<command>` shape is a
        # documented contract, not an implementation detail -- users compose `time_based` covers
        # against these IDs and cannot do that against IDs they can't predict.
        identity[CONF_BUTTON_IDS] = {
            command: ID(
                f"{identity_id}_{command}",
                is_declaration=True,
                type=IOHomeOneWayCommandButton,
            )
            for command in identity[CONF_COMMANDS]
        }
        identity[CONF_LAST_COMMAND_SENSOR_ID] = ID(
            f"{identity_id}_last_1w_command",
            is_declaration=True,
            type=IOHomeOneWayLastCommandTextSensor,
        )
        if identity[CONF_ENROLLMENT]:
            identity[CONF_ENROLL_BUTTON_ID] = ID(
                f"{identity_id}_enroll",
                is_declaration=True,
                type=IOHomeOneWayEnrollButton,
            )

    return config


# cover:/light:/lock:/switch: are the device-bound platforms that can carry an io_device_id: or a
# linked_remotes: entry (platform_common.py). button:/number:/select:/sensor:/text_sensor: are
# either not device-bound or, for this component, hub-level only.
_DEVICE_BOUND_DOMAINS = ("cover", "light", "lock", "switch")


def _collect_declared_device_addresses(full_config):
    """Map every node ID declared as an `io_device_id:` or a bare `linked_remotes:` entry, across
    every `home_io_control` entity in `full_config`, to a human-readable owner string.

    Only entries with `platform: home_io_control` are considered — the domains in
    _DEVICE_BOUND_DOMAINS are shared with every other component that provides a cover/light/
    lock/switch platform, and those have nothing to do with this component's address space.
    `class:` linked_remotes entries name a device *type*, not a node, so they carry no address to
    collide with and are skipped.

    CONF_IO_DEVICE_ID/CONF_LINKED_REMOTES are imported locally from platform_common rather than at
    module level: platform_common imports back from this module (`from . import ...`), so a
    module-level import here would be circular. By the time this function actually runs (final
    validation, after every used platform module has already been imported), the cycle has
    already resolved and the import is a plain cache hit.
    """
    from .platform_common import CONF_IO_DEVICE_ID, CONF_LINKED_REMOTES

    addresses = {}
    for domain in _DEVICE_BOUND_DOMAINS:
        for entry in full_config.get(domain, []):
            if not isinstance(entry, dict) or entry.get(CONF_PLATFORM) != "home_io_control":
                continue
            owner_name = entry.get(CONF_NAME) or entry.get(CONF_ID) or "<unnamed>"
            device_id = entry.get(CONF_IO_DEVICE_ID)
            if device_id:
                addresses[device_id] = f"{domain} '{owner_name}' io_device_id"
            for remote in entry.get(CONF_LINKED_REMOTES, []):
                if remote.startswith("class:"):
                    continue
                addresses[remote] = f"{domain} '{owner_name}' linked_remotes"
    return addresses


def _final_validate_oneway_controller_addresses(config):
    """Extend the oneway_controllers address-collision check to addresses declared outside
    `home_io_control:` — a `linked_remotes:` entry or an `io_device_id:` on some other entity in
    this same YAML.

    Runs as FINAL_VALIDATE_SCHEMA rather than inside _validate_oneway_controllers() because only
    final validation has access to the full cross-component config (`fv.full_config`) —
    `cover:`/`light:`/`lock:`/`switch:` entries are validated independently of
    `home_io_control:`'s own CONFIG_SCHEMA and are not visible to it. By this point
    _validate_oneway_controllers() has already run, so every identity's `node_id` (derived or
    explicit) is resolved.
    """
    identities = config.get(CONF_ONEWAY_CONTROLLERS, [])
    if not identities:
        return config

    declared = _collect_declared_device_addresses(fv.full_config.get())
    for identity in identities:
        _reject_node_id_collision(
            identity[CONF_ID], identity[CONF_NODE_ID], declared, identity.get(CONF_NODE_ID_DERIVED)
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate_oneway_controller_addresses


def validate_device_id(value):
    """Validate io_device_id as exactly 6 hex characters (3 bytes)."""
    value = cv.string_strict(value).upper()
    if len(value) != 6:
        raise cv.Invalid("Device ID must be exactly 6 hex characters (3 bytes)")
    try:
        int(value, 16)
    except ValueError:
        raise cv.Invalid("Device ID must be valid hexadecimal")
    return value


def inherit_esphome_device(companion_config, parent_config):
    """Propagate the parent entity's ESPHome sub-device (YAML `device_id:`) onto a hand-built
    companion config dict, so the companion entity groups under the same HA device as its parent.

    Lives here rather than in platform_common.py: it is needed by button.py's pairing-result
    sensor, which is not a device-bound platform and would otherwise have to import the whole
    platform-schema module for a four-line helper. platform_common.py re-exports it so cover.py's
    existing import keeps working.

    Companion entities (diagnostic sensors, cover favorite/vent buttons, ...) are built from
    dicts fed straight to e.g. new_text_sensor()/new_button() rather than through the platform's
    own cv.Schema(), so they never go through ENTITY_BASE_SCHEMA and never pick up `device_id:`
    on their own. esphome.core.entity_helpers.setup_entity() reads it with
    `config.get(CONF_DEVICE_ID)`, a truthiness check, so an explicit `None` and an absent key
    behave identically; omitted here rather than set to None just to keep the dict shape
    identical to a companion with no sub-device at all.

    Deliberately not called anywhere for the hub-level dynamic entities (1W identity buttons/
    sensors, the arming switches, LR1121 firmware controls, tuning numbers/selects): none of their
    parent configs carry a `device_id:` schema slot, since those entities aren't attached to a
    single cover/light/switch/lock to inherit one from. `device_id:` grouping is scoped to the
    four device-bound platforms; hub-level entities always live on ESPHome's main device.
    """
    if (esphome_device_id := parent_config.get(CONF_DEVICE_ID)) is not None:
        companion_config[CONF_DEVICE_ID] = esphome_device_id
    return companion_config


def validate_linked_remote_entry(value):
    """Validate a linked_remotes entry: either a device ID or 'class:<device_type>'.

    The class form matches how 1W remotes address a typed broadcast (e.g. "all awnings")
    rather than a single node, so one entry can cover many same-type devices without
    enumerating each one. Shares _resolve_device_type_token() with validate_device_type()
    so a type without a named YAML alias yet (e.g. discovered via pairing) can still be
    class-linked. Normalized to 'class:0x<HH>' (uppercase hex) so wire_device_binding() can
    parse the type directly without a second DEVICE_TYPE_OPTIONS lookup; bare device IDs are
    validated exactly as before and behave identically.
    """
    if isinstance(value, str) and value.lower().startswith("class:"):
        type_token = value.split(":", 1)[1].strip().lower()
        try:
            type_value = _resolve_device_type_token(type_token)
        except ValueError as err:
            raise cv.Invalid(
                f"Unknown device class '{type_token}' in linked_remotes; expected one of: "
                + ", ".join(sorted(DEVICE_TYPE_OPTIONS))
                + ", or a raw integer such as 0x14"
            ) from err
        return f"class:0x{type_value:02X}"
    return validate_device_id(value)


def validate_status_poll_interval(value):
    """Validate status_poll_interval is at least MIN_STATUS_POLL_INTERVAL_MS."""
    value = cv.positive_time_period_milliseconds(value)
    if value.total_milliseconds < MIN_STATUS_POLL_INTERVAL_MS:
        raise cv.Invalid(
            f"status_poll_interval must be at least {MIN_STATUS_POLL_INTERVAL_MS}ms"
        )
    return value


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(IOHomeControlComponent),
            cv.Required(CONF_RST_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_DIO0_PIN): pins.internal_gpio_input_pin_schema,
            cv.Optional(CONF_DIO4_PIN): pins.internal_gpio_input_pin_schema,
            # The chip's IRQ line: SX1262's DIO1, or LR1121's DIO9
            cv.Optional(CONF_DIO1_PIN): pins.internal_gpio_input_pin_schema,
            cv.Optional(CONF_BUSY_PIN): pins.internal_gpio_input_pin_schema,
            cv.Required(CONF_NODE_ID): validate_node_id,
            cv.Required(CONF_SYSTEM_KEY): cv.sensitive(validate_system_key),
            cv.Optional(CONF_TX_POWER, default=17): cv.int_range(min=0, max=22),
            cv.Optional(CONF_PA_PIN, default="BOOST"): cv.enum(
                PA_PIN_OPTIONS, upper=True
            ),
            cv.Required(CONF_RADIO_TYPE): cv.enum(RADIO_TYPE_OPTIONS, lower=True),
            cv.Optional(CONF_FEM_EN_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_VFEM_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_FEM_PA_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_TCXO_VOLTAGE, default="1_8V"): cv.enum(
                TCXO_VOLTAGE_OPTIONS, upper=True
            ),
            cv.Optional(CONF_EXPOSED_SENDERS, default=[]): cv.ensure_list(
                validate_device_id
            ),
            cv.Optional(CONF_ACCEPT_FOREIGN_PAIRING, default=False): cv.boolean,
            cv.Optional(CONF_RECOVER_ONEWAY_KEY, default=False): cv.boolean,
            cv.Optional(CONF_ONEWAY_CONTROLLERS, default=[]): cv.ensure_list(
                ONEWAY_CONTROLLER_SCHEMA
            ),
            cv.Optional(CONF_DIAGNOSTIC_PROBES, default=False): cv.boolean,
            cv.Optional(CONF_LR1121_FIRMWARE_UPDATE): LR1121_FIRMWARE_UPDATE_SCHEMA,
            cv.Optional(tuning_module.CONF_TUNING): tuning_module.TUNING_CONFIG_SCHEMA,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(spi.spi_device_schema(True, 8e6, "mode0")),
    _inject_accept_foreign_pairing_switch_id,
    _inject_recover_oneway_key_switch_id,
    _validate_oneway_controllers,
    _validate_lr1121_firmware_update,
)


async def to_code(config):
    # Hub-level management actions and result events are compiled behind native API
    # feature flags. Home IO Control enables the required compile-time switches here
    # so users only need a normal `api:` block in YAML.
    # ESPHome 2026.x additionally gates user-defined actions behind
    # USE_API_USER_DEFINED_ACTIONS.
    cg.add_define("USE_API_USER_DEFINED_ACTIONS")
    cg.add_define("USE_API_CUSTOM_SERVICES")
    cg.add_define("USE_API_HOMEASSISTANT_SERVICES")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await spi.register_spi_device(var, config)

    rst_pin = await cg.gpio_pin_expression(config[CONF_RST_PIN])
    cg.add(var.set_rst_pin(rst_pin))

    if CONF_DIO0_PIN in config:
        dio0_pin = await cg.gpio_pin_expression(config[CONF_DIO0_PIN])
        cg.add(var.set_dio0_pin(dio0_pin))

    if CONF_DIO4_PIN in config:
        dio4_pin = await cg.gpio_pin_expression(config[CONF_DIO4_PIN])
        cg.add(var.set_dio4_pin(dio4_pin))

    if CONF_DIO1_PIN in config:
        dio1_pin = await cg.gpio_pin_expression(config[CONF_DIO1_PIN])
        cg.add(var.set_dio1_pin(dio1_pin))

    if CONF_BUSY_PIN in config:
        busy_pin = await cg.gpio_pin_expression(config[CONF_BUSY_PIN])
        cg.add(var.set_busy_pin(busy_pin))

    if CONF_FEM_EN_PIN in config:
        fem_en_pin = await cg.gpio_pin_expression(config[CONF_FEM_EN_PIN])
        cg.add(var.set_fem_en_pin(fem_en_pin))

    if CONF_VFEM_PIN in config:
        vfem_pin = await cg.gpio_pin_expression(config[CONF_VFEM_PIN])
        cg.add(var.set_vfem_pin(vfem_pin))

    if CONF_FEM_PA_PIN in config:
        fem_pa_pin = await cg.gpio_pin_expression(config[CONF_FEM_PA_PIN])
        cg.add(var.set_fem_pa_pin(fem_pa_pin))

    cg.add(var.set_node_id(config[CONF_NODE_ID]))
    cg.add(var.set_system_key(config[CONF_SYSTEM_KEY]))
    cg.add(var.set_tx_power(config[CONF_TX_POWER]))
    cg.add(var.set_pa_pin(config[CONF_PA_PIN]))

    cg.add(var.set_radio_type(config[CONF_RADIO_TYPE]))

    cg.add(var.set_tcxo_voltage(config[CONF_TCXO_VOLTAGE]))

    for sender_id in config[CONF_EXPOSED_SENDERS]:
        cg.add(var.add_exposed_sender(sender_id))

    for identity in config[CONF_ONEWAY_CONTROLLERS]:
        cg.add(
            var.add_oneway_controller(
                oneway_controller_expression(identity, config[CONF_NODE_ID])
            )
        )
        await _create_oneway_controller_entities(identity, var)

    if config[CONF_ACCEPT_FOREIGN_PAIRING]:
        await _create_hub_arming_switch(
            config,
            var,
            cls=IOHomeAcceptForeignPairingSwitch,
            id_key=CONF_ACCEPT_FOREIGN_PAIRING_SWITCH_ID,
            name="Recover System Key",
        )

    if config[CONF_RECOVER_ONEWAY_KEY]:
        await _create_hub_arming_switch(
            config,
            var,
            cls=IOHomeRecoverOneWayKeySwitch,
            id_key=CONF_RECOVER_ONEWAY_KEY_SWITCH_ID,
            name="Recover 1W Controller Key",
        )

    cg.add(var.set_diagnostic_probes_enabled(config[CONF_DIAGNOSTIC_PROBES]))

    if CONF_LR1121_FIRMWARE_UPDATE in config:
        await _create_lr1121_firmware_update(config, var)

    if tuning_module.CONF_TUNING in config:
        await tuning_module.to_code(config[tuning_module.CONF_TUNING], var)


async def _create_oneway_controller_entities(identity, var):
    """Create one identity's command buttons and its "Last 1W Command" diagnostic sensor.

    Same normalization as the hub-level switches below: run a bare {id, name} dict through the
    platform's own schema so it carries the entity/component defaults register_*() require.

    Entity names derive from the identity handle and the command ("awning_remote" + "open" ->
    "Awning Remote Open"), mirroring how the cover's favourite/vent companions derive theirs. The
    *IDs* follow the documented `<identity_id>_<command>` rule instead, because those are what a
    `time_based` cover composes against.
    """
    friendly_identity = identity[CONF_ID].replace("_", " ").title()

    for command, button_id in identity[CONF_BUTTON_IDS].items():
        entity_config = button_component.button_schema(
            IOHomeOneWayCommandButton,
        ).extend(cv.COMPONENT_SCHEMA)(
            {
                CONF_ID: button_id,
                CONF_NAME: f"{friendly_identity} {command.replace('_', ' ').title()}",
            }
        )
        entity = await button_component.new_button(entity_config)
        await cg.register_component(entity, entity_config)
        cg.add(entity.set_parent(var))
        cg.add(entity.set_controller_id(identity[CONF_ID]))
        cg.add(entity.set_action(ONEWAY_COMMANDS[command]))

    # Always created, even with no buttons: an identity driven only by the
    # `oneway_set_position` action still needs somewhere to show what it sent, and with no reply
    # frame this sensor is the only place that can ever appear.
    sensor_config = text_sensor_component.text_sensor_schema(
        IOHomeOneWayLastCommandTextSensor,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ).extend(cv.COMPONENT_SCHEMA)(
        {
            CONF_ID: identity[CONF_LAST_COMMAND_SENSOR_ID],
            CONF_NAME: f"{friendly_identity} Last 1W Command",
        }
    )
    sensor = await text_sensor_component.new_text_sensor(sensor_config)
    await cg.register_component(sensor, sensor_config)
    cg.add(sensor.set_parent(var))
    cg.add(sensor.set_controller_id(identity[CONF_ID]))

    if identity[CONF_ENROLLMENT]:
        # entity_category: config, not a switch behind an arming flag: the receiver's own 2s PROG
        # hold is the real interlock -- a hub cannot enroll into a device nobody has walked up to
        # (ADR 0026). ADR 0021's bootloader precedent doesn't apply here (that ADR is for an
        # irreversible write; un-enrollment exists here as a rollback path).
        #
        # No "(May Replace Existing Remotes)" caveat: enrolling a new identity onto a device is
        # additive, not destructive -- an existing remote keeps working alongside a newly enrolled
        # hub. Un-enrollment (`0x39`, the `oneway_remove_controller` action) has not been confirmed
        # to work on real hardware yet -- see that action's own doxygen (management_actions.h) --
        # so "reversible" is the design intent, not yet a demonstrated fact.
        enroll_config = button_component.button_schema(
            IOHomeOneWayEnrollButton,
            entity_category=ENTITY_CATEGORY_CONFIG,
        ).extend(cv.COMPONENT_SCHEMA)(
            {
                CONF_ID: identity[CONF_ENROLL_BUTTON_ID],
                CONF_NAME: f"{friendly_identity} Enroll 1W Controller",
            }
        )
        enroll_entity = await button_component.new_button(enroll_config)
        await cg.register_component(enroll_entity, enroll_config)
        cg.add(enroll_entity.set_parent(var))
        cg.add(enroll_entity.set_controller_id(identity[CONF_ID]))


async def _create_hub_arming_switch(config, var, *, cls, id_key, name):
    """Create a hub-level arming switch (key extraction or key adoption).

    Mirrors tuning.py's _create_number()/_create_select(): normalize a bare {id, name} dict
    through switch_schema()+COMPONENT_SCHEMA so it carries the entity/component defaults
    register_switch()/register_component() require, matching the neighboring pattern rather than
    hand-assembling a config dict shape of its own.

    ALWAYS_OFF is a security property, not a UX default: every switch built here arms a window
    (foreign-key extraction or 1W key adoption) that must never come back armed after a reboot.
    """
    entity_config = switch_component.switch_schema(
        cls,
        default_restore_mode="ALWAYS_OFF",  # never auto-arm after a reboot
        entity_category=ENTITY_CATEGORY_CONFIG,
    ).extend(cv.COMPONENT_SCHEMA)(
        {
            CONF_ID: config[id_key],
            CONF_NAME: name,
        }
    )
    entity = await switch_component.new_switch(entity_config)
    await cg.register_component(entity, entity_config)
    cg.add(entity.set_parent(var))


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
