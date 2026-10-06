## @file
## @brief Field validators and option tables for the ``home_io_control:`` hub block.
## @ingroup hioc_codegen
##
## Radio type, PA pin, TCXO voltage and FEM profile tables (``validate_radio_transport`` checks
## that an SPI radio has its chip-select pin, ``validate_fem`` a profile's required pins and
## TX-power ceiling), the device-type and manufacturer
## tables, and the validators for node IDs, system keys, device IDs, linked remotes and
## status-poll intervals that the hub schema and the platform modules share.
##
## DEVICE_TYPE_OPTIONS, MANUFACTURER_OPTIONS, PA_PIN_OPTIONS, TCXO_VOLTAGE_OPTIONS and
## FEM_TX_POWER_MAX_QUIET mirror C++ tables; the ``tests/sync/`` host tests and
## ``make yaml-emitter-sync`` read them from this file by name.

import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_CS_PIN, CONF_DEVICE_ID

from .hub_names import (
    CONF_FEM,
    CONF_FEM_EN_PIN,
    CONF_FEM_PA_PIN,
    CONF_RADIO_TYPE,
    CONF_TX_POWER,
    CONF_VFEM_PIN,
    MIN_RAIN_SENSOR_POLL_INTERVAL_MS,
    MIN_STATUS_POLL_INTERVAL_MS,
    home_io_control_ns,
)

_LOGGER = logging.getLogger(__name__)


PA_PIN_OPTIONS = {
    "BOOST": 0x80,
    "RFO": 0x00,
}

RADIO_TYPE_OPTIONS = {
    "sx1276": "sx1276",
    "sx1262": "sx1262",
    "lr1121": "lr1121",
}

# Radio types whose chip sits on the SPI bus, so the hub registers as an SPI device for them and
# needs its chip-select pin. Every radio type is one today; a radio reached some other way is
# left out of this set.
SPI_RADIO_TYPES = frozenset({"sx1276", "sx1262", "lr1121"})

# 0-based voltage enum shared verbatim by the SX1262 SetDIO3AsTCXOCtrl and LR1121 SetTcxoMode
# commands (0x00 = 1.6 V .. 0x07 = 3.3 V; Semtech SX1261/2 datasheet Table 13-35). Both drivers
# pass the generated integer straight through to the chip. The YAML strings ("1_8V" etc.) are
# unchanged, so this is not a user-visible config change. "NONE" (0xFF) is a sentinel for boards
# with a bare crystal instead of a TCXO: the driver skips the DIO3/TCXO programming entirely.
TCXO_VOLTAGE_OPTIONS = {
    "1_6V": 0x00,
    "1_7V": 0x01,
    "1_8V": 0x02,
    "2_2V": 0x03,
    "2_4V": 0x04,
    "2_7V": 0x05,
    "3_0V": 0x06,
    "3_3V": 0x07,
    "NONE": 0xFF,
}

# Which RF front-end part is fitted (ADR 0035). Part names, not board names -- board names
# belong to config/boards/heltec-v4-*.yaml. "Part", not "chip": gc1109/kct8103l are each a single
# bare FEM IC, but xy16p35 names an RF module with no single chip inside it independently
# identifiable as "the FEM chip" from anything published -- see FEM_REQUIRED_PINS' own comment and
# radio_interface.h's FemProfile doc block for the fuller explanation. Validated with cv.one_of
# (not cv.enum) so config[CONF_FEM] stays a plain string usable in ordinary Python comparisons in
# validate_fem() below; the mapping to the generated C++ enum happens explicitly in to_code(),
# same pattern as ONEWAY_COMMANDS.
FemProfile = home_io_control_ns.enum("FemProfile", is_class=True)
FEM_PROFILES = {
    "none": FemProfile.NONE,
    "gc1109": FemProfile.GC1109,      # Heltec WiFi LoRa 32 V4.2
    "kct8103l": FemProfile.KCT8103L,  # Heltec WiFi LoRa 32 V4.3 / V4 R8
    "xy16p35": FemProfile.XY16P35,    # LilyGO T-Beam 1W SX1262
}

# Config keys a `fem:` profile actually drives. Every profile requires vfem_pin (the FEM/module
# power enable) and fem_pa_pin (the mode pin whose active-during-TX level is profile-dependent --
# see FemProfile's own doc comment in radio_interface.h). fem_en_pin (CSD, a secondary chip-enable
# some front-end parts expose) is required only where the part actually has one: GC1109 and
# KCT8103L do, XY16P35 does not (LilyGO's own datasheet names only "LDO EN" and "LNA Ctrl" --
# no third pin).
# `fem:` never supplies the GPIO numbers themselves; those always come from the board's own
# config/boards/*.yaml, exactly like every other radio pin in this schema.
FEM_REQUIRED_PINS = {
    "gc1109": (CONF_VFEM_PIN, CONF_FEM_EN_PIN, CONF_FEM_PA_PIN),
    "kct8103l": (CONF_VFEM_PIN, CONF_FEM_EN_PIN, CONF_FEM_PA_PIN),
    "xy16p35": (CONF_VFEM_PIN, CONF_FEM_PA_PIN),
}

# Highest tx_power setting whose estimated antenna-port power still stays at or under the
# 868 MHz SRD ERP limit (+14 dBm); validate_fem() warns on anything above it. Derived from the
# same low-drive net-gain figures as SX1262_FEM_GAIN_*_DB in radio_sx1262.cpp (GC1109 ~+11 dB ->
# 14 dBm at tx_power 3, 15 dBm at 4; KCT8103L ~+13 dB -> 14 dBm at tx_power 1, 15 dBm at 2;
# XY16P35 ~+14 dB (measured; the PA's own nominal spec is +12dB) -> 14 dBm at tx_power 0, 15 dBm
# at 1). Kept here by hand since Python and C++ share no header; the driver's own boot-time
# ESP_LOGW carries the authoritative per-tx_power estimate and its uncertainty.
FEM_TX_POWER_MAX_QUIET = {
    "gc1109": 3,
    "kct8103l": 1,
    "xy16p35": 0,
}


def validate_radio_transport(config):
    """Require the SPI device keys for a radio on the SPI bus. The schema takes them as optional
    (``spi.spi_device_schema(False, ...)``) so that the requirement follows ``radio_type`` here,
    in one place, rather than being hard-wired into the schema for every radio."""
    radio_type = config[CONF_RADIO_TYPE]
    if radio_type in SPI_RADIO_TYPES and CONF_CS_PIN not in config:
        raise cv.Invalid(
            f"radio_type: {radio_type} is an SPI radio and needs cs_pin: (its chip-select GPIO); "
            "a board package in config/boards/ sets it for you",
            path=[CONF_CS_PIN],
        )
    return config


def validate_fem(config):
    """Cross-key validation for `fem:` (ADR 0035): which front-end part's control behaviour the
    SX1262 driver applies to fem_pa_pin (and, for the two parts that have one, fem_en_pin), never
    which GPIO numbers to use for them -- those are always the board package's job, the same as
    every other radio pin. Requires radio_type: sx1262 (the raw FEM pins are silently ignored on
    every other chip -- select_and_construct_radio_() only ever hands them to RadioSX1262 -- so a
    mismatched radio_type would configure hardware the driver never drives), and that every pin
    this profile's behaviour needs is actually present, naming exactly which is missing rather
    than leaving the driver to silently no-op on a null pin. Also warns when tx_power looks likely
    to push this FEM's antenna-port power over a typical 868 MHz SRD limit -- radio_sx1262.cpp's
    own init() logs the actual per-part estimate this warning can only gesture at without knowing
    the front-end part's exact gain table here too.
    """
    profile = config[CONF_FEM]
    if profile == "none":
        return config

    if config[CONF_RADIO_TYPE] != "sx1262":
        raise cv.Invalid(
            f"fem: {profile} requires radio_type: sx1262 -- the front-end module pins are only "
            "wired to the SX1262 driver"
        )

    missing = [key for key in FEM_REQUIRED_PINS[profile] if key not in config]
    if missing:
        raise cv.Invalid(
            f"fem: {profile} requires {', '.join(missing)} to be set -- see "
            "docs/hardware.md#front-end-module-fem-support for what your board needs"
        )

    if config[CONF_TX_POWER] > FEM_TX_POWER_MAX_QUIET[profile]:
        _LOGGER.warning(
            "fem: %s together with tx_power: %d -- a front-end module turns tx_power into a "
            "much larger antenna-port power than the bare SX1262 would radiate, and at this "
            "setting the estimate is likely over a typical 868 MHz SRD ERP limit (+14 dBm). The "
            "driver logs its own per-profile estimate (and its uncertainty) at boot -- check it, "
            "and prefer tx_power: %d or lower (what this profile's own board package ships) "
            "until you have measured the actual radiated power for your board",
            profile,
            config[CONF_TX_POWER],
            FEM_TX_POWER_MAX_QUIET[profile],
        )

    return config


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
    "bioclimatic_pergola": 0x1D,
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
ONEWAY_WIRE_PROFILE_MANUFACTURERS = {
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


def _validate_min_interval(value, minimum_ms, key):
    """Parse a time period and reject anything shorter than minimum_ms, naming the key."""
    value = cv.positive_time_period_milliseconds(value)
    if value.total_milliseconds < minimum_ms:
        raise cv.Invalid(f"{key} must be at least {minimum_ms}ms")
    return value


def validate_status_poll_interval(value):
    """Validate status_poll_interval is at least MIN_STATUS_POLL_INTERVAL_MS."""
    return _validate_min_interval(
        value, MIN_STATUS_POLL_INTERVAL_MS, "status_poll_interval"
    )


def validate_rain_sensor_poll_interval(value):
    """Validate rain_sensor_poll_interval is at least MIN_RAIN_SENSOR_POLL_INTERVAL_MS."""
    return _validate_min_interval(
        value, MIN_RAIN_SENSOR_POLL_INTERVAL_MS, "rain_sensor_poll_interval"
    )
