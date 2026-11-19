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
## DEVICE_TYPE_OPTIONS, DEVICE_MODEL_OPTIONS, DEVICE_FEATURES (with FEATURES, EXPERIMENTAL_KINDS and
## NAMED_POSITION_FEATURES), DEVICE_READINGS (with READING_FEATURE_HINTS and READING_REMOVE_HINTS), MANUFACTURER_OPTIONS, PA_PIN_OPTIONS, TCXO_VOLTAGE_OPTIONS and
## FEM_TX_POWER_MAX_QUIET mirror C++ tables; the ``tests/sync/`` host tests and
## ``make yaml-emitter-sync`` read them from this file by name.

import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import (
    CONF_CS_PIN,
    CONF_DEVICE_ID,
    CONF_ID,
    CONF_NAME,
    CONF_PLATFORM,
    DEVICE_CLASS_AWNING,
    DEVICE_CLASS_BLIND,
    DEVICE_CLASS_CURTAIN,
    DEVICE_CLASS_DOOR,
    DEVICE_CLASS_GARAGE,
    DEVICE_CLASS_GATE,
    DEVICE_CLASS_SHADE,
    DEVICE_CLASS_SHUTTER,
    DEVICE_CLASS_WINDOW,
)

from .hub_names import (
    CONF_FEM,
    CONF_FEM_EN_PIN,
    CONF_FEM_PA_PIN,
    CONF_IO_DEVICE_ID,
    CONF_RADIO_TYPE,
    CONF_TX_POWER,
    CONF_VFEM_PIN,
    DEVICE_BOUND_DOMAINS,
    MIN_READING_POLL_INTERVAL_MS,
    MIN_STATUS_POLL_INTERVAL_MS,
    MIN_DEVICE_READING_INTERVAL_MS,
    MIN_STATUS_REFRESH_INTERVAL_MS,
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
    "domestic_hot_water": 0x33,
    "electrical_heater": 0x34,
}

# The Home Assistant device class a cover of each declared kind gets when its YAML sets no
# `device_class:` (cover.py injects it; an explicit value, including "", wins). Python-only: no C++
# code reads a device class. A kind whose class would be a guess has no entry.
DEFAULT_COVER_DEVICE_CLASS = {
    "garage_opener": DEVICE_CLASS_GARAGE,
    "gate_opener": DEVICE_CLASS_GATE,
    "rolling_door_opener": DEVICE_CLASS_DOOR,
    "window_opener": DEVICE_CLASS_WINDOW,
    "sliding_window": DEVICE_CLASS_WINDOW,
    "roller_shutter": DEVICE_CLASS_SHUTTER,
    "dual_roller_shutter": DEVICE_CLASS_SHUTTER,
    "swinging_shutter": DEVICE_CLASS_SHUTTER,
    "interior_venetian_blind": DEVICE_CLASS_BLIND,
    "exterior_venetian_blind": DEVICE_CLASS_BLIND,
    "louvre_blind": DEVICE_CLASS_BLIND,
    "vertical_interior_blind": DEVICE_CLASS_SHADE,
    "curtain_track": DEVICE_CLASS_CURTAIN,
    "horizontal_awning": DEVICE_CLASS_AWNING,
    "vertical_exterior_awning": DEVICE_CLASS_AWNING,
}

# === Device capabilities ===
#
# Python's mirror of the capability set each node-type table row carries (proto_device_features.h,
# proto_device_types.cpp), so the codegen can decide which companions and keys a declared kind has. A
# capability is its name; the codegen asks for a name (platform_common.has_feature(), require_feature()).
# The C++ table is authoritative; tests/sync/device_features_sync_test.cpp checks the names against the
# C++ enumeration and the set of every (type, subtype) row. Literals only: the sync test parses this
# file and reads no expressions.

# Every capability, in the order of the C++ enumeration DeviceFeature.
FEATURES = (
    "MAIN_PARAMETER_ONLY",
    "LEVELS",
    "TILT",
    "FAVORITE",
    "VENT",
    "PARTIAL",
    "PEDESTRIAN",
    "EXTENDED_POSITION_FRAME",
    "WINDOW_COMPANIONS",
    "INVERTED",
    "WATER_HEATER",
    "NIGHT_LIGHT",
    "TEMPERATURE_RANGE",
    "CENTRAL_HEATING_MODE",
    "OCCUPANCY",
    "ENERGY_METER",
    "RUN_TIMERS",
    "REPORTS_HEATING_LEVEL",
    "NO_STATUS_READ",
    "HEAT_PUMP_ENERGY",
    "HEAT_PUMP_OUTDOOR_TEMPERATURE",
)

# Python's mirror of the table of named device readings (proto_device_readings.cpp), by the enumerator name of
# DeviceReading: the capability a device needs ("feature"), where the value comes from ("source": "register" for one
# private register of a heater or a water heater, or a part of one, "derived" for a value worked out from other
# readings, "data_object" for a slice of a data object read on a schedule, "status" for the main parameter of a status
# reply, "status_parameter" for a functional parameter of one, "event" for the data of a device event answered to a
# subscription sent on a schedule, "limitation" for the answer to a limitation read sent on a schedule,
# "status_battery" for the battery byte of a status reply), the platform that offers the reading's YAML key or
# companion ("platform": "climate", "water_heater", "sensor" or "cover"), the registers the hub's poll reads for it
# ("registers", by DeviceRegister enumerator name; none for a data object, an event, a limitation or a status reply), and what the codegen builds
# for it: the entity kind ("sensor", "binary_sensor", "switch" or "text_sensor"; the C++ value kind decides which fits),
# the Home Assistant metadata of the entity ("unit", "accuracy", "device_class", "state_class") and how it is declared:
# the YAML "key" under the main entity (None where no key declares it), or, for an entity every device of the
# capability gets without asking, a "companion" (the suffix of its name after the main entity's), its "companion_id"
# (the suffix of its ID, after the main entity's) and, optionally, "disabled_by_default". The optional "icon" and
# "entity_category" complete the entity's metadata. A reading has a key or a companion, never both. A cover's reading
# has an "opt_in_key" instead of a capability: the YAML key of the cover whose presence (and, for `battery`, a true
# value) declares the reading and creates its companion; where the reading is read on a schedule ("event" or
# "limitation"), the key's value is the interval. A reading with a key or a companion is offered by its platform only, so the two "energy" keys
# (a heater's and a water heater's) are two rows on two platforms. A "data_object" reading is on the `sensor:` platform;
# it also carries "update_interval_ms", the interval its YAML key reads at when the entry sets none. The recipe, offset,
# byte range and codec of a data object, and the value names of a named reading, are C++ only.
# tests/sync/device_features_sync_test.cpp checks names, capabilities, source kinds and registers against the C++ table,
# the platform against the row's capability and source kind, and each entity kind against the C++ value kind; the other
# entity fields are Python-only, since C++ never needs them. One entry per line: the sync test parses this file and
# reads no expressions.
DEVICE_READINGS = {
    "CENTRAL_HEATING_MODE": {"feature": "CENTRAL_HEATING_MODE", "source": "register", "platform": "climate", "registers": ("CENTRAL_HEATING_MODE",), "key": "central_heating_mode", "entity": "binary_sensor"},
    "OCCUPANCY_DETECTION": {"feature": "OCCUPANCY", "source": "register", "platform": "climate", "registers": ("OCCUPANCY_DETECTION",), "key": "occupancy_detection", "entity": "switch"},
    "OCCUPANCY": {"feature": "OCCUPANCY", "source": "register", "platform": "climate", "registers": ("PRESENCE",), "key": "occupancy", "entity": "binary_sensor", "device_class": "occupancy"},
    "OPEN_WINDOW_DETECTION": {"feature": "WINDOW_COMPANIONS", "source": "register", "platform": "climate", "registers": ("OPEN_WINDOW_DETECTION",), "key": None, "entity": "switch", "companion": "Open Window Detection", "companion_id": "window_detection_switch", "disabled_by_default": True},
    "ENERGY_TOTAL": {"feature": "ENERGY_METER", "source": "register", "platform": "climate", "registers": ("ENERGY_TOTAL",), "key": "energy", "entity": "sensor", "unit": "kWh", "accuracy": 0, "device_class": "energy", "state_class": "total_increasing"},
    "BOOST_DURATION": {"feature": "RUN_TIMERS", "source": "register", "platform": "climate", "registers": ("BOOST_DURATION",), "key": "boost_duration", "entity": "sensor", "unit": "min", "accuracy": 0, "device_class": "duration"},
    "DRYING_DURATION": {"feature": "RUN_TIMERS", "source": "register", "platform": "climate", "registers": ("DRYING_DURATION",), "key": "drying_duration", "entity": "sensor", "unit": "min", "accuracy": 0, "device_class": "duration"},
    "BOOST_REMAINING": {"feature": "RUN_TIMERS", "source": "register", "platform": "climate", "registers": ("BOOST_REMAINING",), "key": None, "entity": None},
    "DRYING_REMAINING": {"feature": "RUN_TIMERS", "source": "register", "platform": "climate", "registers": ("DRYING_REMAINING",), "key": None, "entity": None},
    "REMAINING_TIME": {"feature": "RUN_TIMERS", "source": "derived", "platform": "climate", "registers": ("BOOST_REMAINING", "DRYING_REMAINING"), "key": "remaining_time", "entity": "sensor", "unit": "min", "accuracy": 0, "device_class": "duration", "state_class": "measurement"},
    "ROOM_TEMPERATURE": {"feature": None, "source": "register", "platform": "climate", "registers": ("ROOM_TEMPERATURE",), "key": None, "entity": None},
    "SETPOINT": {"feature": None, "source": "register", "platform": "climate", "registers": ("COMFORT_SETPOINT",), "key": None, "entity": None},
    "MAX_SETPOINT": {"feature": None, "source": "register", "platform": "climate", "registers": ("MAX_SETPOINT",), "key": None, "entity": None},
    "MODE": {"feature": None, "source": "register", "platform": "climate", "registers": ("OPERATING_MODE",), "key": None, "entity": None},
    "REGULATOR_STATE": {"feature": None, "source": "register", "platform": "climate", "registers": ("REGULATOR_STATE",), "key": None, "entity": None},
    "WINDOW_OPEN": {"feature": "WINDOW_COMPANIONS", "source": "register", "platform": "climate", "registers": ("WINDOW_OPEN",), "key": None, "entity": "binary_sensor", "device_class": "window", "companion": "Window", "companion_id": "window_sensor"},
    "TOWEL_MODE": {"feature": None, "source": "register", "platform": "climate", "registers": ("TOWEL_MODE",), "key": None, "entity": None},
    "TEMPORARY_STATE": {"feature": None, "source": "register", "platform": "climate", "registers": ("TEMPORARY_STATE",), "key": None, "entity": None},
    "WORKING_RATE": {"feature": None, "source": "register", "platform": "climate", "registers": ("WORKING_RATE",), "key": None, "entity": None},
    "TOWEL_TEMPERATURE_IN_FORCE": {"feature": None, "source": "register", "platform": "climate", "registers": ("TOWEL_TEMPERATURE_IN_FORCE",), "key": None, "entity": None},
    "TEMPERATURE_IN_FORCE": {"feature": None, "source": "register", "platform": "climate", "registers": ("TEMPERATURE_IN_FORCE",), "key": None, "entity": None},
    "DEROGATED_SETPOINT": {"feature": None, "source": "register", "platform": "climate", "registers": ("DEROGATED_SETPOINT",), "key": None, "entity": None},
    "NIGHT_LIGHT": {"feature": "NIGHT_LIGHT", "source": "register", "platform": "climate", "registers": ("NIGHT_LIGHT",), "key": None, "entity": None},
    "NIGHT_LIGHT_BRIGHTNESS": {"feature": "NIGHT_LIGHT", "source": "register", "platform": "climate", "registers": ("NIGHT_LIGHT_BRIGHTNESS",), "key": None, "entity": None},
    "HEATING_LEVEL": {"feature": "REPORTS_HEATING_LEVEL", "source": "status", "platform": "climate", "registers": (), "key": "heating_level", "entity": "text_sensor"},
    "STATUS_SETPOINT": {"feature": None, "source": "status", "platform": "climate", "registers": (), "key": None, "entity": None},
    "HOT_WATER_TEMPERATURE": {"feature": "WATER_HEATER", "source": "register", "platform": "water_heater", "registers": ("DHW_WATER_TEMPERATURE",), "key": None, "entity": None},
    "HOT_WATER_MODE": {"feature": "WATER_HEATER", "source": "register", "platform": "water_heater", "registers": ("DHW_MODE",), "key": None, "entity": None},
    "HOT_WATER_HEATING": {"feature": "WATER_HEATER", "source": "register", "platform": "water_heater", "registers": ("DHW_HEATING",), "key": None, "entity": None},
    "HOT_WATER_ELECTRIC_BACKUP": {"feature": "WATER_HEATER", "source": "register", "platform": "water_heater", "registers": ("DHW_HEATING",), "key": "electric_backup_running", "entity": "binary_sensor", "device_class": "running"},
    "HOT_WATER_HEAT_PUMP": {"feature": "WATER_HEATER", "source": "register", "platform": "water_heater", "registers": ("DHW_HEATING",), "key": "heat_pump_running", "entity": "binary_sensor", "device_class": "running"},
    "HOT_WATER_BOOST_DURATION": {"feature": "WATER_HEATER", "source": "register", "platform": "water_heater", "registers": ("DHW_BOOST_DURATION",), "key": None, "entity": None},
    "HOT_WATER_AWAY_DURATION": {"feature": "WATER_HEATER", "source": "register", "platform": "water_heater", "registers": ("DHW_AWAY_DURATION",), "key": None, "entity": None},
    "HOT_WATER_ENERGY": {"feature": "WATER_HEATER", "source": "register", "platform": "water_heater", "registers": ("DHW_ENERGY",), "key": "energy", "entity": "sensor", "unit": "kWh", "accuracy": 3, "device_class": "energy", "state_class": "total_increasing"},
    "HOT_WATER_BRAND": {"feature": "WATER_HEATER", "source": "register", "platform": "water_heater", "registers": ("DHW_BRAND",), "key": None, "entity": None},
    "HOT_WATER_CAPABILITIES": {"feature": "WATER_HEATER", "source": "status_parameter", "platform": "water_heater", "registers": (), "key": None, "entity": None},
    "HOT_WATER_BOOST": {"feature": "WATER_HEATER", "source": "status_parameter", "platform": "water_heater", "registers": (), "key": None, "entity": None},
    "HOT_WATER_AWAY": {"feature": "WATER_HEATER", "source": "status_parameter", "platform": "water_heater", "registers": (), "key": None, "entity": None},
    "RAIN": {"feature": None, "source": "limitation", "platform": "cover", "registers": (), "key": None, "opt_in_key": "rain_sensor_poll_interval", "min_interval_ms": MIN_READING_POLL_INTERVAL_MS, "entity": "binary_sensor", "device_class": "moisture", "companion": "Rain sensor", "companion_id": "rain_sensor"},
    "SUN_LEVEL": {"feature": None, "source": "event", "platform": "cover", "registers": (), "key": None, "opt_in_key": "sun_level_poll_interval", "min_interval_ms": MIN_READING_POLL_INTERVAL_MS, "entity": "sensor", "unit": "lx", "accuracy": 0, "device_class": "illuminance", "state_class": "measurement", "icon": "mdi:white-balance-sunny", "companion": "Sun level", "companion_id": "sun_level_sensor"},
    "BATTERY_LEVEL": {"feature": None, "source": "event", "platform": "cover", "registers": (), "key": None, "opt_in_key": "battery_level_poll_interval", "min_interval_ms": MIN_READING_POLL_INTERVAL_MS, "entity": "sensor", "unit": "%", "accuracy": 0, "device_class": "battery", "state_class": "measurement", "companion": "Battery level", "companion_id": "battery_level_sensor"},
    "BATTERY_STATE": {"feature": None, "source": "status_battery", "platform": "cover", "registers": (), "key": None, "opt_in_key": "battery", "entity": "text_sensor", "entity_category": "diagnostic", "icon": "mdi:battery", "companion": "Battery", "companion_id": "battery_sensor", "disabled_by_default": True},
    "BATTERY_LOW": {"feature": None, "source": "derived", "platform": "cover", "registers": (), "key": None, "opt_in_key": "battery", "entity": "binary_sensor", "device_class": "battery", "entity_category": "diagnostic", "companion": "Battery Low", "companion_id": "battery_low_sensor"},
    "HEAT_PUMP_HEATING_ENERGY": {"feature": "HEAT_PUMP_ENERGY", "source": "data_object", "platform": "sensor", "registers": (), "key": "heating_energy", "entity": "sensor", "unit": "kWh", "accuracy": 0, "device_class": "energy", "state_class": "total_increasing", "update_interval_ms": 3600000},
    "HEAT_PUMP_HOT_WATER_ENERGY": {"feature": "HEAT_PUMP_ENERGY", "source": "data_object", "platform": "sensor", "registers": (), "key": "hot_water_energy", "entity": "sensor", "unit": "kWh", "accuracy": 0, "device_class": "energy", "state_class": "total_increasing", "update_interval_ms": 3600000},
    "HEAT_PUMP_OUTDOOR_TEMPERATURE": {"feature": "HEAT_PUMP_OUTDOOR_TEMPERATURE", "source": "data_object", "platform": "sensor", "registers": (), "key": "outdoor_temperature", "entity": "sensor", "unit": "°C", "accuracy": 1, "device_class": "temperature", "state_class": "measurement", "update_interval_ms": 600000},
}

# What to declare when a key of DEVICE_READINGS is used on a kind without its capability, by capability.
READING_FEATURE_HINTS = {
    "WATER_HEATER": "a domestic hot water heater: set io_device_type: domestic_hot_water",
    "CENTRAL_HEATING_MODE": "a towel dryer: set io_device_type: electrical_heater, io_subtype: 1 and io_model: towel_dryer",
    "OCCUPANCY": "a heater with occupancy detection: set io_device_type: electrical_heater, io_subtype: 1 and either no io_model (a radiator) or io_model: heater_with_light",
    "ENERGY_METER": "a heater that counts its energy: set io_device_type: electrical_heater, io_subtype: 1 and either no io_model (a radiator), io_model: towel_dryer or io_model: heater_with_light",
    "RUN_TIMERS": "a towel dryer: set io_device_type: electrical_heater, io_subtype: 1 and io_model: towel_dryer",
    "REPORTS_HEATING_LEVEL": "an electrical heater: set io_device_type: electrical_heater and either io_subtype: 0 without io_model (a plain heater) or io_subtype: 1 with io_model: towel_dryer",
    "HEAT_PUMP_ENERGY": "an Atlantic heat pump: set io_device_type: heat_pump and io_subtype: 1",
    "HEAT_PUMP_OUTDOOR_TEMPERATURE": "an Atlantic heat pump: set io_device_type: heat_pump and io_subtype: 1",
}

# What to do when a key of DEVICE_READINGS is used on a kind of a device type that has the capability on other kinds,
# by capability: the declared kind is the right type, so the user is told to drop the key instead of what to declare.
READING_REMOVE_HINTS = {
    "REPORTS_HEATING_LEVEL": "the hub reads no heating level from this heater; remove {key}:",
}

# The models a (type, subtype) can have below it (proto_device_kinds.h DeviceModel, the model rows of
# proto_device_types.cpp), by their YAML io_model: name. Values mirror the C++ enum;
# tests/sync/device_features_sync_test.cpp checks them. Which (type, subtype) offers a model is the
# "<name>/<subtype>/<model>" key of DEVICE_FEATURES.
DEVICE_MODEL_OPTIONS = {
    "towel_dryer": 1,
    "heater_with_light": 2,
    "pilot_wire_interface": 3,
}

# The capabilities of each row whose type has a YAML name, keyed "<name>" for subtype 0,
# "<name>/<subtype>" for a subtype row and "<name>/<subtype>/<model>" for a model row. A row with no
# capability is left out, except a subtype row under a type that has capabilities (without its explicit
# empty set the lookup would give it the type's capabilities) and a model row (it is how the codegen
# knows the model is offered).
DEVICE_FEATURES = {
    "interior_venetian_blind": frozenset({"LEVELS", "TILT", "FAVORITE"}),
    "roller_shutter": frozenset({"LEVELS", "FAVORITE", "EXTENDED_POSITION_FRAME"}),
    "roller_shutter/1": frozenset({"LEVELS", "FAVORITE", "EXTENDED_POSITION_FRAME"}),
    "roller_shutter/2": frozenset({"LEVELS", "FAVORITE", "EXTENDED_POSITION_FRAME"}),
    "vertical_exterior_awning": frozenset({"LEVELS", "FAVORITE", "EXTENDED_POSITION_FRAME"}),
    "window_opener": frozenset({"LEVELS", "FAVORITE", "VENT", "EXTENDED_POSITION_FRAME"}),
    "window_opener/1": frozenset({"LEVELS", "FAVORITE", "VENT", "EXTENDED_POSITION_FRAME"}),
    "garage_opener": frozenset({"MAIN_PARAMETER_ONLY", "LEVELS", "PARTIAL"}),
    "garage_opener/58": frozenset({"MAIN_PARAMETER_ONLY", "PARTIAL"}),
    "light/1": frozenset({"LEVELS"}),
    "light/2": frozenset({"LEVELS"}),
    "light/58": frozenset({"MAIN_PARAMETER_ONLY"}),
    "gate_opener": frozenset({"LEVELS", "FAVORITE", "PEDESTRIAN", "EXTENDED_POSITION_FRAME"}),
    "gate_opener/58": frozenset({"MAIN_PARAMETER_ONLY", "FAVORITE", "PEDESTRIAN"}),
    "rolling_door_opener": frozenset({"LEVELS", "FAVORITE", "EXTENDED_POSITION_FRAME"}),
    "vertical_interior_blind": frozenset({"LEVELS", "FAVORITE", "EXTENDED_POSITION_FRAME"}),
    "dual_roller_shutter": frozenset({"LEVELS", "FAVORITE"}),
    "horizontal_awning": frozenset({"LEVELS", "FAVORITE", "EXTENDED_POSITION_FRAME", "INVERTED"}),
    "horizontal_awning/1": frozenset({"LEVELS", "FAVORITE", "EXTENDED_POSITION_FRAME", "INVERTED"}),
    "exterior_venetian_blind": frozenset({"LEVELS", "TILT", "FAVORITE", "EXTENDED_POSITION_FRAME"}),
    "louvre_blind": frozenset({"LEVELS", "TILT", "FAVORITE", "EXTENDED_POSITION_FRAME"}),
    "curtain_track": frozenset({"LEVELS", "FAVORITE", "EXTENDED_POSITION_FRAME"}),
    "heating_temperature_interface": frozenset({"TEMPERATURE_RANGE"}),
    "exterior_heating": frozenset({"MAIN_PARAMETER_ONLY", "LEVELS"}),
    "exterior_heating/58": frozenset({"MAIN_PARAMETER_ONLY"}),
    "swinging_shutter": frozenset({"LEVELS", "FAVORITE", "EXTENDED_POSITION_FRAME"}),
    "swinging_shutter/1": frozenset({"LEVELS", "FAVORITE", "EXTENDED_POSITION_FRAME"}),
    "swinging_shutter/58": frozenset({"MAIN_PARAMETER_ONLY", "FAVORITE"}),
    "sliding_window": frozenset({"LEVELS", "FAVORITE", "EXTENDED_POSITION_FRAME"}),
    "sliding_window/1": frozenset({"LEVELS", "FAVORITE", "EXTENDED_POSITION_FRAME"}),
    "bioclimatic_pergola": frozenset({"LEVELS", "FAVORITE", "EXTENDED_POSITION_FRAME"}),
    "domestic_hot_water": frozenset({"WATER_HEATER"}),
    "electrical_heater": frozenset({"REPORTS_HEATING_LEVEL"}),
    "electrical_heater/1": frozenset({"WINDOW_COMPANIONS", "OCCUPANCY", "ENERGY_METER"}),
    "electrical_heater/1/towel_dryer": frozenset({"CENTRAL_HEATING_MODE", "ENERGY_METER", "RUN_TIMERS", "REPORTS_HEATING_LEVEL"}),
    "electrical_heater/1/heater_with_light": frozenset({"WINDOW_COMPANIONS", "NIGHT_LIGHT", "OCCUPANCY", "ENERGY_METER"}),
    "electrical_heater/0/pilot_wire_interface": frozenset(),
    "heat_pump/1": frozenset({"HEAT_PUMP_ENERGY", "HEAT_PUMP_OUTDOOR_TEMPERATURE", "NO_STATUS_READ"}),
}

# The kinds no capture or report has confirmed on a real device (device_type_flags::EXPERIMENTAL in
# proto_device_types.h), keyed like DEVICE_FEATURES but without models. The flag names its row only: a
# subtype without a row of its own does not inherit its type's.
EXPERIMENTAL_KINDS = frozenset(
    {
        "light/1",
        "light/2",
        "gate_opener/58",
        "lock/1",
        "lock/2",
        "on_off_switch/2",
        "heat_pump/1",
        "heat_pump/2",
        "swinging_shutter/58",
        "sliding_window/1",
        "domestic_hot_water",
    }
)

# The named positions (proto_device_types.h NAMED_POSITIONS), keyed by the CoverCommand name
# cover_command_name() returns, with the capability a device needs for each. Each companion button of
# cover.py is created from the capability here.
NAMED_POSITION_FEATURES = {
    "FAVORITE": "FAVORITE",
    "VENT": "VENT",
    "PEDESTRIAN": "PEDESTRIAN",
    "PARTIAL": "PARTIAL",
}

# Subtype that marks a device taking the main parameter only, on any node type.
SUBTYPE_MAIN_PARAMETER_ONLY = 58

_DEVICE_TYPE_NAMES = {value: name for name, value in DEVICE_TYPE_OPTIONS.items()}


def device_type_name(device_type):
    """The io_device_type name of a node type value, or None for one YAML has no name for."""
    return _DEVICE_TYPE_NAMES.get(device_type)


_DEVICE_MODEL_NAMES = {value: name for name, value in DEVICE_MODEL_OPTIONS.items()}


def device_models(device_type, subtype=0):
    """The io_model names (io_device_type value, io_subtype) offers, sorted; empty for most kinds."""
    name = device_type_name(device_type)
    if name is None:
        return []
    prefix = f"{name}/{subtype}/"
    return sorted(key[len(prefix):] for key in DEVICE_FEATURES if key.startswith(prefix))


def type_offers_feature(device_type, feature):
    """True when some kind of the node type (a subtype row or a model row of ``DEVICE_FEATURES``) has the capability
    ``feature``; False for a node type YAML has no name for."""
    name = device_type_name(device_type)
    return name is not None and any(
        key.split("/")[0] == name and feature in features for key, features in DEVICE_FEATURES.items()
    )


def check_feature_name(name):
    """Return ``name`` when it is a capability in FEATURES; raise ValueError for a misspelt one."""
    if name not in FEATURES:
        raise ValueError(f"unknown device capability {name!r}")
    return name


def device_features(device_type, subtype=0, model=0):
    """The capabilities of (io_device_type value, io_subtype, io_model value) as a frozenset of names,
    as the C++ device_features() answers.

    The model row; else the exact row; else, for subtype 58, the main-parameter-only variant of the
    type's row (of no capabilities when the type has none); else the type's own row. A node type YAML
    has no name for (a raw value) has no row here, so it has no capabilities beyond the subtype-58
    rule. ``model`` is a DEVICE_MODEL_OPTIONS value, 0 for none; codegen passes the config's model
    through platform_common.config_model().
    """
    name = device_type_name(device_type)
    model_name = _DEVICE_MODEL_NAMES.get(model)
    if name is not None and model_name is not None:
        model_row = DEVICE_FEATURES.get(f"{name}/{subtype}/{model_name}")
        if model_row is not None:
            return model_row
    if name is not None:
        exact = DEVICE_FEATURES.get(name if subtype == 0 else f"{name}/{subtype}")
        if exact is not None:
            return exact
    base = DEVICE_FEATURES.get(name, frozenset()) if name is not None else frozenset()
    if subtype == SUBTYPE_MAIN_PARAMETER_ONLY:
        return (base - {"LEVELS", "TILT", "EXTENDED_POSITION_FRAME"}) | {"MAIN_PARAMETER_ONLY"}
    return base


def kind_is_experimental(device_type, subtype=0):
    """True when no capture or report has confirmed (io_device_type value, io_subtype) on a real device.

    Reads EXPERIMENTAL_KINDS, which names exact rows only: a subtype with no row of its own never
    inherits the marker from its type row, as C++ device_is_experimental().
    """
    name = device_type_name(device_type)
    if name is None:
        return False
    return (name if subtype == 0 else f"{name}/{subtype}") in EXPERIMENTAL_KINDS


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

# Manufacturers for which oneway_controller.h's ONEWAY_WIRE_PROFILES has a real 1W wire profile, by their
# MANUFACTURER_OPTIONS name. tests/sync/oneway_wire_profile_sync_test.cpp checks this tuple against that
# table. Literals only: the sync test parses this file and reads no expressions.
ONEWAY_WIRE_PROFILE_MANUFACTURER_NAMES = ("somfy", "velux")
ONEWAY_WIRE_PROFILE_MANUFACTURERS = {MANUFACTURER_OPTIONS[name] for name in ONEWAY_WIRE_PROFILE_MANUFACTURER_NAMES}


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


def device_model_expression(value):
    """Generate a C++ static_cast expression for a validated DEVICE_MODEL_OPTIONS value."""
    return cg.RawExpression(f"static_cast<esphome::home_io_control::DeviceModel>({value})")


def validate_device_model_name(value):
    """Validate io_model as one of the DEVICE_MODEL_OPTIONS names (lowercase).

    Whether the declared io_device_type / io_subtype offers the model is
    platform_common.validate_device_model()'s job: it needs the whole entry.
    """
    normalized = cv.string_strict(value).strip().lower()
    if normalized not in DEVICE_MODEL_OPTIONS:
        raise cv.Invalid(
            f"Unknown io_model '{value}'; known models: {', '.join(sorted(DEVICE_MODEL_OPTIONS))}"
        )
    return normalized


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


def declared_device_entries(full_config):
    """Yield (domain, entry, owner name) for every `platform: home_io_control` entry of a device-bound domain.

    The domains in DEVICE_BOUND_DOMAINS are shared with every other component that provides such a platform, and
    those entries have nothing to do with this component, so only ours are yielded. The owner name (the entry's
    name, else its ID) is what a whole-YAML check prints to point at the entry.
    """
    for domain in DEVICE_BOUND_DOMAINS:
        for entry in full_config.get(domain, []):
            if not isinstance(entry, dict) or entry.get(CONF_PLATFORM) != "home_io_control":
                continue
            yield domain, entry, entry.get(CONF_NAME) or entry.get(CONF_ID) or "<unnamed>"


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
    """Group a companion entity under its parent entity's ESPHome sub-device (YAML `device_id:`),
    unless the companion names its own.

    Lives here, a leaf module, so any platform module can import it without the whole
    platform-schema module; platform_common.py re-exports it for the platform files.

    A companion's config (a diagnostic sensor, a cover command button, a water heater's energy
    sensor, the heater's night light, ...) comes from its own entity schema, which has no
    `device_id:` default, so without this it would land on ESPHome's main device. A companion the
    user configures as a nested schema may set `device_id:` itself; that choice is kept.
    esphome.core.entity_helpers.setup_entity() reads the key with `config.get(CONF_DEVICE_ID)`, a
    truthiness check, so a parent without a sub-device leaves the key absent rather than None.

    Deliberately not called anywhere for the hub-level dynamic entities (1W identity buttons/
    sensors, the arming switches, LR1121 firmware controls, tuning numbers/selects): none of their
    parent configs carry a `device_id:` schema slot, since those entities aren't attached to a
    single cover/light/switch/lock to inherit one from. `device_id:` grouping is scoped to the
    device-bound platforms; hub-level entities always live on ESPHome's main device.
    """
    if CONF_DEVICE_ID in companion_config:
        return companion_config
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


def validate_reading_poll_interval(reading_name):
    """Return the validator of a reading's poll interval key: its `opt_in_key` and floor in DEVICE_READINGS."""
    row = DEVICE_READINGS[reading_name]
    key = row["opt_in_key"]
    minimum_ms = row["min_interval_ms"]

    def validate(value):
        return _validate_min_interval(value, minimum_ms, key)

    return validate


def reading_poll_interval_schema(platform):
    """The optional poll interval keys of a platform's scheduled readings, from the DEVICE_READINGS rows."""
    return {
        cv.Optional(row["opt_in_key"]): validate_reading_poll_interval(name)
        for name, row in DEVICE_READINGS.items()
        if row["platform"] == platform and "min_interval_ms" in row
    }


def validate_device_reading_interval(value):
    """Validate a data-object reading's update_interval is at least MIN_DEVICE_READING_INTERVAL_MS."""
    return _validate_min_interval(
        value, MIN_DEVICE_READING_INTERVAL_MS, "update_interval"
    )


def validate_status_refresh_interval(value):
    """Validate status_refresh_interval is at least MIN_STATUS_REFRESH_INTERVAL_MS."""
    return _validate_min_interval(
        value, MIN_STATUS_REFRESH_INTERVAL_MS, "status_refresh_interval"
    )
