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
import esphome.final_validate as fv
from esphome.components.spi import CONF_SPI_ID
from esphome.const import CONF_BAUD_RATE, CONF_CS_PIN, CONF_DEVICE_ID, CONF_RX_BUFFER_SIZE, CONF_UART_ID

from .tuning import (
    CONF_COLD_BROADCAST_REPLY_PREAMBLE,
    CONF_LR1121_RESPONSE_PREAMBLE,
    CONF_NORMAL_START_PREAMBLE,
    CONF_SX1262_POST_TX_SETTLE_US,
    CONF_SX1262_RESPONSE_PREAMBLE,
    CONF_SX1276_RESPONSE_PREAMBLE,
    CONF_TUNING,
)
from .hub_names import (
    CONF_BRIDLE_BAND,
    CONF_BUSY_PIN,
    CONF_DIO0_PIN,
    CONF_DIO1_PIN,
    CONF_DIO4_PIN,
    CONF_TCXO_VOLTAGE,
    CONF_FEM,
    CONF_FEM_EN_PIN,
    CONF_FEM_PA_PIN,
    CONF_ONEWAY_CONTROLLERS,
    CONF_RADIO_TYPE,
    CONF_RECOVER_ONEWAY_KEY,
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
    "bridle": "bridle",
}

# Radio types whose chip sits on the SPI bus, so the hub registers as an SPI device for them and
# needs its chip-select pin. A radio reached some other way (`bridle`: a co-processor behind a
# UART) is left out of this set.
SPI_RADIO_TYPES = frozenset({"sx1276", "sx1262", "lr1121"})

# `bridle_band` values: the PHY profile asked of a Bridle co-processor.
BRIDLE_BANDS = ("io_2g4", "io_868")

# UART speeds a Bridle co-processor is spoken to at: the spec's 1 Mbaud by default, and the lower rates
# the spec allows a port to offer as a build option (both sides must be set to the same rate, which the
# protocol cannot negotiate, or the co-processor answers nothing).
BRIDLE_BAUD_RATE = 1000000
BRIDLE_REDUCED_BAUD_RATES = (115200, 230400, 460800, 921600)
BRIDLE_BAUD_RATES = (*BRIDLE_REDUCED_BAUD_RATES, BRIDLE_BAUD_RATE)
# The slowest link a 2.4 GHz band may use (Bridle specification §2.1): another controller's wake train puts a
# frame on the air every millisecond, and each one reaches the hub as a frame_start and an rx event, about 36 bytes,
# which 230400 baud cannot carry in a millisecond, so the co-processor's event queue overflows.
BRIDLE_802154_MIN_BAUD_RATE = 460800
# Receive buffer that holds a burst of frames between two loop() passes at 1 Mbaud (100 bytes/ms).
BRIDLE_MIN_RX_BUFFER = 1024

# The `tx_power` range of the SPI radios (unchanged) and of a bridge (dBm, as the protocol's s8 and
# real 2.4 GHz radios allow; the co-processor clamps it to its radio and reports what it applied).
SPI_TX_POWER_RANGE = (0, 22)
SPI_TX_POWER_DEFAULT = 17
BRIDLE_TX_POWER_RANGE = (-40, 22)

# Tuning keys of an SPI radio's own timing that a bridge cannot honour: the co-processor owns the
# turnaround after a transmission (it waits for its own radio), so the host's value has no effect over
# a bridge whatever the band.
_TUNING_NO_EFFECT_OVER_A_BRIDGE_KEYS = (CONF_SX1262_POST_TX_SETTLE_US,)

# Tuning keys that only mean something to an 868 MHz radio: preamble lengths.
_TUNING_868_ONLY_KEYS = (
    CONF_SX1262_RESPONSE_PREAMBLE,
    CONF_SX1276_RESPONSE_PREAMBLE,
    CONF_LR1121_RESPONSE_PREAMBLE,
    CONF_COLD_BROADCAST_REPLY_PREAMBLE,
    CONF_NORMAL_START_PREAMBLE,
)

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


# Keys that belong to a chip on the SPI bus (or to a front-end around one) and mean nothing to a
# co-processor behind a UART. `tcxo_voltage` and `pa_pin` carry schema defaults, so only the keys
# without one can be rejected by their presence.
_BRIDLE_REJECTED_KEYS = (CONF_CS_PIN, CONF_BUSY_PIN, CONF_DIO0_PIN, CONF_DIO1_PIN, CONF_DIO4_PIN, CONF_TCXO_VOLTAGE)


def validate_radio_transport(config):
    """Require the keys of the way the radio is reached. The schema takes them all as optional
    (``spi.spi_device_schema(False, ...)``, ``uart_id``) so that the requirement follows
    ``radio_type`` here, in one place, rather than being hard-wired into the schema for every radio.

    An SPI radio needs its chip-select pin. A Bridle co-processor needs the UART it is wired to,
    takes none of the chip keys.
    """
    radio_type = config[CONF_RADIO_TYPE]
    if radio_type in SPI_RADIO_TYPES:
        if CONF_UART_ID in config:
            raise cv.Invalid(
                f"uart_id: only applies to radio_type: bridle, not {radio_type}", path=[CONF_UART_ID]
            )
        if CONF_CS_PIN not in config:
            raise cv.Invalid(
                f"radio_type: {radio_type} is an SPI radio and needs cs_pin: (its chip-select GPIO); "
                "a board package in config/boards/ sets it for you",
                path=[CONF_CS_PIN],
            )
        return config

    if CONF_UART_ID not in config:
        raise cv.Invalid(
            "radio_type: bridle needs uart_id: (the uart: block the co-processor is wired to)",
            path=[CONF_UART_ID],
        )
    for key in _BRIDLE_REJECTED_KEYS:
        if key in config:
            raise cv.Invalid(
                f"{key}: does not apply to radio_type: bridle -- the radio is a co-processor behind "
                "the UART, and its board settings belong to its firmware",
                path=[key],
            )
    # spi_device_schema() generates an spi_id whatever the radio; without an spi: bus it would fail
    # validation, and a UART radio registers no SPI device.
    config.pop(CONF_SPI_ID, None)
    tuning = config.get(CONF_TUNING, {})
    band = config.get(CONF_BRIDLE_BAND, "io_2g4")
    if band == "io_2g4":
        for key in _TUNING_868_ONLY_KEYS:
            if key in tuning:
                _LOGGER.warning(
                    "tuning: %s has no effect with bridle_band io_2g4: 2.4 GHz has no preamble to lengthen", key
                )
    for key in _TUNING_NO_EFFECT_OVER_A_BRIDGE_KEYS:
        if key in tuning:
            _LOGGER.warning(
                "tuning: %s has no effect with radio_type: bridle: the co-processor owns the turnaround after a "
                "transmission",
                key,
            )
    return config


def validate_tx_power(config):
    """Give ``tx_power`` its per-radio default and range. The SPI radios keep their 0…22 range and
    default 17 (a PA setting whose meaning differs per chip); a bridge takes any dBm value the
    protocol allows and, when it is unset, leaves the power to the radio, which knows its own
    maximum. Runs before ``validate_fem``, which reads the filled-in value."""
    radio_type = config[CONF_RADIO_TYPE]
    if radio_type not in SPI_RADIO_TYPES:
        return config
    power = config.get(CONF_TX_POWER, SPI_TX_POWER_DEFAULT)
    low, high = SPI_TX_POWER_RANGE
    if not low <= power <= high:
        raise cv.Invalid(
            f"tx_power: {power} is outside {low}..{high} for radio_type: {radio_type}", path=[CONF_TX_POWER]
        )
    config[CONF_TX_POWER] = power
    return config


def final_validate_bridle_uart(config):
    """Check the UART a Bridle co-processor is wired to, which only the whole config can show:
    a rate the co-processor can be built for (1 Mbaud unless it is built for a lower one), at least
    BRIDLE_802154_MIN_BAUD_RATE for the 2.4 GHz band, and a receive buffer that holds the frames of a burst between two loop() passes."""
    if config[CONF_RADIO_TYPE] != "bridle":
        return config
    full_config = fv.full_config.get()
    uart_path = full_config.get_path_for_id(config[CONF_UART_ID])[:-1]
    uart_config = full_config.get_config_for_path(uart_path)
    baud = uart_config[CONF_BAUD_RATE]
    if baud not in BRIDLE_BAUD_RATES:
        raise cv.Invalid(
            f"the UART for radio_type: bridle must run at {BRIDLE_BAUD_RATE} baud (or "
            f"one of {', '.join(str(rate) for rate in BRIDLE_REDUCED_BAUD_RATES)} for a co-processor built "
            f"for it), not {baud}",
            path=[CONF_UART_ID],
        )
    if config.get(CONF_BRIDLE_BAND, "io_2g4") == "io_2g4" and baud < BRIDLE_802154_MIN_BAUD_RATE:
        raise cv.Invalid(
            f"bridle_band io_2g4 needs the UART at {BRIDLE_802154_MIN_BAUD_RATE} baud or more, not {baud}: "
            "another controller's wake train sends a frame every millisecond, and a slower link cannot pass "
            "them on, so the co-processor drops frames",
            path=[CONF_UART_ID],
        )
    if baud != BRIDLE_BAUD_RATE:
        _LOGGER.warning(
            "uart at %d baud: the Bridle co-processor must be built for that speed, or it answers nothing",
            baud,
        )
    if uart_config.get(CONF_RX_BUFFER_SIZE, 0) < BRIDLE_MIN_RX_BUFFER:
        _LOGGER.warning(
            "uart rx_buffer_size below %d bytes: at %d baud a loop() pass can leave more than that "
            "unread, and frames are lost. Set rx_buffer_size: %d on the uart",
            BRIDLE_MIN_RX_BUFFER,
            baud,
            BRIDLE_MIN_RX_BUFFER,
        )
    return config


def final_validate_no_oneway_on_io_2g4(config):
    """Refuse one-way transmit on the 2.4 GHz band.

    No one-way frame has ever been seen on 2.4 GHz, so `oneway_controllers:` and
    `recover_oneway_key:` would transmit frames no receiver there is known to act on. A clear
    configuration error is better than a command that silently does nothing. Receiving one-way
    frames (`linked_remotes:`, `exposed_senders:`) stays allowed.
    """
    if config[CONF_RADIO_TYPE] != "bridle" or config.get(CONF_BRIDLE_BAND, "io_2g4") != "io_2g4":
        return config
    for key in (CONF_ONEWAY_CONTROLLERS, CONF_RECOVER_ONEWAY_KEY):
        if config.get(key):
            raise cv.Invalid(
                f"{key}: is not available with bridle_band io_2g4: one-way frames have never been seen "
                "on 2.4 GHz, so a one-way command would go out and nothing would act on it. "
                "Remove it, or use a 868 MHz radio for one-way devices",
                path=[key],
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


# The wire carries the node type in ten bits (0..1023). The names below are the types a YAML entity
# can declare; the table that names every node type and subtype is proto_device_types.cpp, and
# tests/sync/device_type_sync_test.cpp keeps the two in step.
DEVICE_TYPE_MAX = 0x3FF

DEVICE_TYPE_OPTIONS = {
    "unknown": 0x00,
    "interior_venetian_blind": 0x01,
    "roller_shutter": 0x02,
    "vertical_exterior_awning": 0x03,
    "window_opener": 0x04,
    "garage_opener": 0x05,
    "light": 0x06,
    "gate_opener": 0x07,
    "rolling_door_opener": 0x08,
    "lock": 0x09,
    "vertical_interior_blind": 0x0A,
    "dual_roller_shutter": 0x0D,
    "heating_temperature_interface": 0x0E,
    "on_off_switch": 0x0F,
    "horizontal_awning": 0x10,
    "exterior_venetian_blind": 0x11,
    "louvre_blind": 0x12,
    "curtain_track": 0x13,
    "exterior_heating": 0x15,
    "heat_pump": 0x16,
    "intrusion_alarm": 0x17,
    "swinging_shutter": 0x18,
    "sliding_window": 0x1B,
    "bioclimatic_pergola": 0x1D,
    "electrical_heater": 0x34,
}

# Heating profiles (proto_heating_profiles.cpp) that have a window sensor and an open-window
# detection switch, as {io_device_type name: io_subtype}. climate.py generates those two companions
# for exactly these devices; tests/sync/heating_profile_sync_test.cpp keeps the table in step with
# the C++ profile features.
HEATING_WINDOW_PROFILES = {
    "electrical_heater": 1,
}


def heating_profile_has_window(device_type, subtype):
    """True when the heating profile of (io_device_type value, io_subtype) has the window companions."""
    return any(
        DEVICE_TYPE_OPTIONS[name] == device_type and profile_subtype == subtype
        for name, profile_subtype in HEATING_WINDOW_PROFILES.items()
    )


# Device-type names that were renamed. Each maps to the message the validator raises, so a config
# that still uses one fails with the name to switch to rather than a generic "unknown name".
RENAMED_DEVICE_TYPES = {
    "screen": (
        "device type 'screen' no longer exists: type 11 is a secure configuration device, not an "
        "actuator. Use the type Scan Paired Devices reports for your device, usually "
        "'vertical_exterior_awning' or 'vertical_interior_blind'"
    ),
    "blind": "device type 'blind' was renamed: use 'vertical_interior_blind' instead",
    "venetian_blind": "device type 'venetian_blind' was renamed: use 'interior_venetian_blind' instead",
    "awning": "device type 'awning' was renamed: use 'vertical_exterior_awning' instead",
    "dual_shutter": "device type 'dual_shutter' was renamed: use 'dual_roller_shutter' instead",
    "external_venetian_blind": (
        "device type 'external_venetian_blind' was renamed: use 'exterior_venetian_blind' instead"
    ),
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
    """Resolve a lowercase, stripped device-type token (name or raw int/hex string) to 0-1023.

    Single source of truth for the "named value from DEVICE_TYPE_OPTIONS, else raw integer"
    acceptance rule shared by validate_device_type() (io_device_type) and
    validate_linked_remote_entry() (the class:<device_type> linked-remotes form) — both accept
    the exact same set of device-type spellings, so the lookup lives here once.
    @raises ValueError if token is neither a known name nor a parseable integer.
    @raises cv.Invalid if token is a renamed device-type name (naming the new one), or parses as an
    integer but is out of range 0-1023.
    """
    if token in RENAMED_DEVICE_TYPES:
        raise cv.Invalid(RENAMED_DEVICE_TYPES[token])
    return _resolve_named_or_raw_token(token, DEVICE_TYPE_OPTIONS, DEVICE_TYPE_MAX)


def validate_device_type(value):
    """Validate io_device_type as a named string or integer 0-1023."""
    if isinstance(value, int):
        return cv.int_range(min=0, max=DEVICE_TYPE_MAX)(value)

    if isinstance(value, str):
        normalized = cv.string_strict(value).strip().lower()
        try:
            return _resolve_device_type_token(normalized)
        except ValueError as err:
            raise cv.Invalid(
                "Device type must be a known name or an integer in the range 0..1023 (for example 0x11)"
            ) from err

    raise cv.Invalid(
        "Device type must be a known name or an integer in the range 0..1023"
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
