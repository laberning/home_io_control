## @file
## @brief ESPHome tuning schema and code generation for Home IO Control.
## @ingroup hioc_codegen
##
## Provides the YAML ``tuning:`` block under ``home_io_control:`` and optionally
## generates Home Assistant ``number`` and ``select`` entities so users can adjust
## pairing and radio parameters at runtime. Tuned values are volatile and reset on
## every boot.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number, select
from esphome.const import CONF_ID, CONF_NAME, ENTITY_CATEGORY_CONFIG
from esphome.core import ID

home_io_control_ns = cg.esphome_ns.namespace("home_io_control")

CONF_TUNING = "tuning"
CONF_UI_CONTROLS = "ui_controls"

# Fixed ID prefix for the generated tuning companion entities (see _inject_tuning_companion_ids).
_COMPANION_ID_BASE = "home_io_control"

# --- Radio / physical layer ---
CONF_SX1262_RX_BANDWIDTH = "sx1262_rx_bandwidth"
CONF_SX1262_RESPONSE_PREAMBLE = "sx1262_response_preamble"
CONF_SX1262_POST_TX_SETTLE_US = "sx1262_post_tx_settle_us"
CONF_SX1276_RX_BANDWIDTH = "sx1276_rx_bandwidth"
CONF_SX1276_RESPONSE_PREAMBLE = "sx1276_response_preamble"
CONF_SX1276_DISCOVERY_HOP_SLICE_MS = "sx1276_discovery_hop_slice_ms"
CONF_SX1262_DISCOVERY_HOP_SLICE_MS = "sx1262_discovery_hop_slice_ms"
CONF_LR1121_RX_BANDWIDTH = "lr1121_rx_bandwidth"
CONF_LR1121_RESPONSE_PREAMBLE = "lr1121_response_preamble"
CONF_LR1121_POST_TX_SETTLE_US = "lr1121_post_tx_settle_us"
CONF_LR1121_DISCOVERY_HOP_SLICE_MS = "lr1121_discovery_hop_slice_ms"
CONF_LBT_MAX_RETRIES = "lbt_max_retries"
CONF_LBT_RSSI_THRESHOLD_DBM = "lbt_rssi_threshold_dbm"

# --- Pairing protocol ---
CONF_PAIRING_DISCOVERY_COMMANDS = "pairing_discovery_commands"
CONF_PAIRING_DISCOVERY_DESTINATION = "pairing_discovery_destination"
CONF_PAIRING_DISCOVERY_PAYLOAD = "pairing_discovery_payload"
CONF_PAIRING_DISCOVERY_LOW_POWER = "pairing_discovery_low_power"
CONF_PAIRING_DISCOVERY_WAIT_MS = "pairing_discovery_wait_ms"
CONF_PAIRING_DISCOVERY_INITIAL_DWELL_MS = "pairing_discovery_initial_dwell_ms"
CONF_PAIRING_KEY_EXCHANGE_RETRIES = "pairing_key_exchange_retries"

# C++ type references
TuningConfig = home_io_control_ns.class_("TuningConfig")
IOHomeTuningNumber = home_io_control_ns.class_(
    "IOHomeTuningNumber", number.Number, cg.Component
)
IOHomeTuningSelect = home_io_control_ns.class_(
    "IOHomeTuningSelect", select.Select, cg.Component
)

# C++ enums. These stringify to their fully-qualified C++ names (e.g.
# esphome::home_io_control::SX1262RxBandwidth::BW_117_3_KHZ) so generated code compiles in
# the global-namespace setup() function.
SX1262RxBandwidth = home_io_control_ns.enum("SX1262RxBandwidth", is_class=True)
SX1276RxBandwidth = home_io_control_ns.enum("SX1276RxBandwidth", is_class=True)
LR1121RxBandwidth = home_io_control_ns.enum("LR1121RxBandwidth", is_class=True)
DiscoveryCommand = home_io_control_ns.enum("DiscoveryCommand", is_class=True)

# Map each YAML option string to its C++ enum value. Options are bare kHz numbers (the "kHz"
# unit lives in the entity name) for uniformity with the numeric parameters.
SX1262_BANDWIDTH_OPTIONS = {
    "58.6": SX1262RxBandwidth.BW_58_6_KHZ,
    "78.2": SX1262RxBandwidth.BW_78_2_KHZ,
    "117.3": SX1262RxBandwidth.BW_117_3_KHZ,
    "156.2": SX1262RxBandwidth.BW_156_2_KHZ,
    "187.2": SX1262RxBandwidth.BW_187_2_KHZ,
}

SX1276_BANDWIDTH_OPTIONS = {
    "20.8": SX1276RxBandwidth.BW_20_8_KHZ,
    "41.7": SX1276RxBandwidth.BW_41_7_KHZ,
    "62.5": SX1276RxBandwidth.BW_62_5_KHZ,
    "83.3": SX1276RxBandwidth.BW_83_3_KHZ,
    "125.0": SX1276RxBandwidth.BW_125_0_KHZ,
}

# LR1121 has its own register encoding, distinct from SX1262's (see tuning_config.h
# LR1121RxBandwidth — two of the five values borrowed from SX1262 turned out wrong for this
# chip). Includes two narrower options (39.0/46.9kHz) not offered for SX1262, added to get
# closer to SX1276's real-hardware-validated 41.7kHz default.
LR1121_BANDWIDTH_OPTIONS = {
    "39.0": LR1121RxBandwidth.BW_39_0_KHZ,
    "46.9": LR1121RxBandwidth.BW_46_9_KHZ,
    "58.6": LR1121RxBandwidth.BW_58_6_KHZ,
    "78.2": LR1121RxBandwidth.BW_78_2_KHZ,
    "117.3": LR1121RxBandwidth.BW_117_3_KHZ,
    "156.2": LR1121RxBandwidth.BW_156_2_KHZ,
    "187.2": LR1121RxBandwidth.BW_187_2_KHZ,
}

DISCOVERY_COMMAND_OPTIONS = {
    "0x28": DiscoveryCommand.DISCOVER,
    "0x2A": DiscoveryCommand.DISCOVER_SPE,
    "0x2E": DiscoveryCommand.DISCOVER_ALT,
}

# Home Assistant `select` entities are single-choice, but the discovery phase can send an
# ordered list of commands. These comma-separated presets expose the useful combinations as
# selectable options; the C++ dispatch (update_tuning_select) parses them back into the vector.
# Individual commands plus the Tahoma-style combinations (which send 0x28/0x2A/0x2E together).
DISCOVERY_COMMAND_PRESETS = [
    "0x28",  # default 2W discovery
    "0x2E",  # alternate discovery (the most common thing to try for a stuck device)
    "0x2A",  # SPE / sub-device discovery (niche)
    "0x28,0x2E",  # both broadcasts
    "0x28,0x2A,0x2E",  # all three (Tahoma-style)
]

DISCOVERY_DESTINATION_OPTIONS = ["auto", "0x00003B", "0x00003F"]

PAIRING_DISCOVERY_PAYLOAD_OPTIONS = ["none", "0x00"]

def _id_key(param_key):
    """Config-dict key under which a parameter's injected companion entity ID is stored."""
    return f"_{param_key}_id"


# UI entity names. Radio params are prefixed "Radio" and pairing params "Pairing" so that,
# under Home Assistant's Configuration section (entity_category=config), the two logical groups
# cluster together alphabetically.
UI_NAMES = {
    CONF_SX1262_RX_BANDWIDTH: "Radio SX1262 RX Bandwidth (kHz)",
    CONF_SX1262_RESPONSE_PREAMBLE: "Radio SX1262 Response Preamble",
    CONF_SX1262_POST_TX_SETTLE_US: "Radio SX1262 Post-TX Settle",
    CONF_SX1276_RX_BANDWIDTH: "Radio SX1276 RX Bandwidth (kHz)",
    CONF_SX1276_RESPONSE_PREAMBLE: "Radio SX1276 Response Preamble",
    CONF_SX1276_DISCOVERY_HOP_SLICE_MS: "Radio SX1276 Discovery Hop Slice",
    CONF_SX1262_DISCOVERY_HOP_SLICE_MS: "Radio SX1262 Discovery Hop Slice",
    CONF_LR1121_RX_BANDWIDTH: "Radio LR1121 RX Bandwidth (kHz)",
    CONF_LR1121_RESPONSE_PREAMBLE: "Radio LR1121 Response Preamble",
    CONF_LR1121_POST_TX_SETTLE_US: "Radio LR1121 Post-TX Settle",
    CONF_LR1121_DISCOVERY_HOP_SLICE_MS: "Radio LR1121 Discovery Hop Slice",
    CONF_LBT_MAX_RETRIES: "Radio LBT Max Retries",
    CONF_LBT_RSSI_THRESHOLD_DBM: "Radio LBT RSSI Threshold",
    CONF_PAIRING_DISCOVERY_COMMANDS: "Pairing Discovery Commands",
    CONF_PAIRING_DISCOVERY_DESTINATION: "Pairing Discovery Destination",
    CONF_PAIRING_DISCOVERY_PAYLOAD: "Pairing Discovery Payload",
    CONF_PAIRING_DISCOVERY_LOW_POWER: "Pairing Discovery Low Power",
    CONF_PAIRING_DISCOVERY_WAIT_MS: "Pairing Discovery Wait",
    CONF_PAIRING_DISCOVERY_INITIAL_DWELL_MS: "Pairing Discovery Initial Dwell",
    CONF_PAIRING_KEY_EXCHANGE_RETRIES: "Pairing Key Exchange Retries",
}

# Numeric parameters: key -> (min, max, step, unit). Single source of truth for both the
# YAML validation range and the Home Assistant `number` entity bounds, so the two cannot drift.
_NUMBER_PARAMS = {
    CONF_SX1262_RESPONSE_PREAMBLE: (32, 256, 1, "B"),
    CONF_SX1262_POST_TX_SETTLE_US: (0, 2000, 10, "µs"),
    CONF_SX1276_RESPONSE_PREAMBLE: (8, 256, 1, "B"),
    CONF_SX1276_DISCOVERY_HOP_SLICE_MS: (5, 200, 1, "ms"),
    CONF_SX1262_DISCOVERY_HOP_SLICE_MS: (50, 500, 1, "ms"),
    # LR1121 numeric ranges reuse the SX1262 bounds — same chip-family physical constraints
    # (design doc §3.2: "seed every timing/tuning default from the validated SX1262 values").
    CONF_LR1121_RESPONSE_PREAMBLE: (32, 256, 1, "B"),
    CONF_LR1121_POST_TX_SETTLE_US: (0, 2000, 10, "µs"),
    CONF_LR1121_DISCOVERY_HOP_SLICE_MS: (50, 500, 1, "ms"),
    CONF_LBT_MAX_RETRIES: (0, 10, 1, ""),
    CONF_LBT_RSSI_THRESHOLD_DBM: (-95, -70, 1, "dBm"),
    CONF_PAIRING_DISCOVERY_WAIT_MS: (500, 5000, 50, "ms"),
    CONF_PAIRING_DISCOVERY_INITIAL_DWELL_MS: (0, 500, 10, "ms"),
    CONF_PAIRING_KEY_EXCHANGE_RETRIES: (1, 5, 1, ""),
}

# Select parameters: key -> ordered option list. Single source of truth for both the
# companion-ID injection and the entity creation.
_SELECT_OPTIONS = {
    CONF_PAIRING_DISCOVERY_COMMANDS: DISCOVERY_COMMAND_PRESETS,
    CONF_PAIRING_DISCOVERY_DESTINATION: DISCOVERY_DESTINATION_OPTIONS,
    CONF_PAIRING_DISCOVERY_PAYLOAD: PAIRING_DISCOVERY_PAYLOAD_OPTIONS,
    CONF_PAIRING_DISCOVERY_LOW_POWER: ["Off", "On"],
    CONF_SX1262_RX_BANDWIDTH: list(SX1262_BANDWIDTH_OPTIONS),
    CONF_SX1276_RX_BANDWIDTH: list(SX1276_BANDWIDTH_OPTIONS),
    CONF_LR1121_RX_BANDWIDTH: list(LR1121_BANDWIDTH_OPTIONS),
}


def _one_of_string(param_name, options, coerce_number=False):
    """Build a validator that requires a string value present in `options`.

    Works for both list and dict `options` (dict membership tests the keys), so a single
    factory replaces a hand-written validator per option set while keeping a per-parameter
    error message. With `coerce_number`, a numeric YAML value (e.g. ``117.3``) is accepted and
    normalized to its string form, so bare-number options do not force the user to quote them.
    """

    def validate(value):
        if coerce_number and isinstance(value, (int, float)) and not isinstance(value, bool):
            value = str(value)
        value = cv.string_strict(value)
        if value not in options:
            raise cv.Invalid(f"{param_name} must be one of {list(options)}")
        return value

    return validate


_validate_bandwidth = _one_of_string(
    CONF_SX1262_RX_BANDWIDTH, SX1262_BANDWIDTH_OPTIONS, coerce_number=True
)
_validate_sx1276_bandwidth = _one_of_string(
    CONF_SX1276_RX_BANDWIDTH, SX1276_BANDWIDTH_OPTIONS, coerce_number=True
)
_validate_lr1121_bandwidth = _one_of_string(
    CONF_LR1121_RX_BANDWIDTH, LR1121_BANDWIDTH_OPTIONS, coerce_number=True
)
_validate_discovery_command = _one_of_string(
    f"{CONF_PAIRING_DISCOVERY_COMMANDS} entries", DISCOVERY_COMMAND_OPTIONS
)
_validate_discovery_destination = _one_of_string(
    CONF_PAIRING_DISCOVERY_DESTINATION, DISCOVERY_DESTINATION_OPTIONS
)
_validate_discovery_payload = _one_of_string(
    CONF_PAIRING_DISCOVERY_PAYLOAD, PAIRING_DISCOVERY_PAYLOAD_OPTIONS
)


def _parse_destination_to_bytes(value):
    """Convert an explicit destination string like '0x00003B' to a list of 3 byte values."""
    digits = value[2:]  # strip the '0x' prefix
    return [int(digits[i : i + 2], 16) for i in range(0, 6, 2)]


def _parse_payload(value):
    """Convert a payload option string like '0x00' to its numeric byte value."""
    return int(value, 16)


# Tuning sub-schema (imported and extended by the hub __init__.py).
#
# Only `ui_controls` carries a Python default (it is a feature toggle, not a tunable). Every
# tunable is a plain cv.Optional with NO default: when a key is omitted, the C++ TuningConfig
# field keeps its canonical default from proto_frame.h, which is the single source of truth.
# to_code() only overrides fields that are actually present in the validated config.
TUNING_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_UI_CONTROLS, default=False): cv.boolean,
        cv.Optional(CONF_SX1262_RX_BANDWIDTH): _validate_bandwidth,
        cv.Optional(CONF_SX1276_RX_BANDWIDTH): _validate_sx1276_bandwidth,
        cv.Optional(CONF_LR1121_RX_BANDWIDTH): _validate_lr1121_bandwidth,
        cv.Optional(CONF_PAIRING_DISCOVERY_COMMANDS): cv.All(
            cv.ensure_list(_validate_discovery_command),
            cv.Length(min=1),
        ),
        cv.Optional(CONF_PAIRING_DISCOVERY_DESTINATION): _validate_discovery_destination,
        cv.Optional(CONF_PAIRING_DISCOVERY_PAYLOAD): _validate_discovery_payload,
        cv.Optional(CONF_PAIRING_DISCOVERY_LOW_POWER): cv.boolean,
        # Numeric parameters share their range with the number-entity bounds via _NUMBER_PARAMS.
        **{
            cv.Optional(key): cv.int_range(min=lo, max=hi)
            for key, (lo, hi, _step, _unit) in _NUMBER_PARAMS.items()
        },
    }
)


def _inject_tuning_companion_ids(config):
    """Declare companion entity IDs for tuning UI controls.

    ESPHome 2026.x sizes the runtime component vector from the number of IDs
    declared during validation. If UI entities were created only in to_code(),
    they could be silently dropped. This post-validator injects declared IDs
    before the vector is sized.
    """
    if not config[CONF_UI_CONTROLS]:
        return config

    # The tuning block is a sub-schema of home_io_control and does not carry the parent's
    # CONF_ID, so the companion entity IDs use a fixed prefix. It only needs to be unique
    # among the generated tuning entities.
    base = _COMPANION_ID_BASE

    for key in _SELECT_OPTIONS:
        config[_id_key(key)] = ID(f"{base}_{key}", is_declaration=True, type=IOHomeTuningSelect)
    for key in _NUMBER_PARAMS:
        config[_id_key(key)] = ID(f"{base}_{key}", is_declaration=True, type=IOHomeTuningNumber)

    return config


TUNING_CONFIG_SCHEMA = cv.All(TUNING_SCHEMA, _inject_tuning_companion_ids)


def _cpp_bool(value):
    """Format a Python bool as a C++ boolean literal."""
    return "true" if value else "false"


def _assign(struct, field, value):
    """Emit a `struct.field = value;` C++ assignment statement at codegen time.

    ESPHome's MockObj does not support Python attribute assignment, so struct fields
    are populated with raw assignment statements. `value` must already be valid C++
    (an int, or a string such as an enum value or brace-init list).
    """
    cg.add(cg.RawExpression(f"{struct}.{field} = {value}"))


def _apply_tuning_config(config, var):
    """Generate C++ code that builds a TuningConfig from the validated YAML.

    Only keys the user actually provided are emitted; every omitted key keeps the
    default baked into the C++ TuningConfig struct (sourced from proto_frame.h), so
    the defaults are never restated here.
    """
    tuning_id = cv.declare_id(TuningConfig)("tuning_config")
    tuning = cg.new_variable(tuning_id, cg.RawExpression(f"{TuningConfig}()"))

    # Presence of the block is what activates the override layer.
    _assign(tuning, "active", "true")

    # --- Parameters needing custom handling (enum/list/byte/bool) ---
    if CONF_SX1262_RX_BANDWIDTH in config:
        _assign(
            tuning,
            "sx1262_rx_bandwidth",
            SX1262_BANDWIDTH_OPTIONS[config[CONF_SX1262_RX_BANDWIDTH]],
        )

    if CONF_SX1276_RX_BANDWIDTH in config:
        _assign(
            tuning,
            "sx1276_rx_bandwidth",
            SX1276_BANDWIDTH_OPTIONS[config[CONF_SX1276_RX_BANDWIDTH]],
        )

    if CONF_LR1121_RX_BANDWIDTH in config:
        _assign(
            tuning,
            "lr1121_rx_bandwidth",
            LR1121_BANDWIDTH_OPTIONS[config[CONF_LR1121_RX_BANDWIDTH]],
        )

    # Ordered discovery commands. Clear the struct default before appending so a
    # user-provided list replaces it rather than extending it.
    if CONF_PAIRING_DISCOVERY_COMMANDS in config:
        cg.add(tuning.pairing_discovery_commands.clear())
        for cmd in config[CONF_PAIRING_DISCOVERY_COMMANDS]:
            cg.add(
                tuning.pairing_discovery_commands.push_back(
                    DISCOVERY_COMMAND_OPTIONS[cmd]
                )
            )

    if CONF_PAIRING_DISCOVERY_DESTINATION in config:
        dest = config[CONF_PAIRING_DISCOVERY_DESTINATION]
        if dest == "auto":
            _assign(tuning, "pairing_discovery_destination_auto", "true")
        else:
            _assign(tuning, "pairing_discovery_destination_auto", "false")
            b = _parse_destination_to_bytes(dest)
            _assign(
                tuning,
                "pairing_discovery_destination",
                f"{{0x{b[0]:02X}, 0x{b[1]:02X}, 0x{b[2]:02X}}}",
            )

    if CONF_PAIRING_DISCOVERY_PAYLOAD in config:
        payload = config[CONF_PAIRING_DISCOVERY_PAYLOAD]
        if payload == "none":
            _assign(tuning, "pairing_discovery_payload_enabled", "false")
        else:
            _assign(tuning, "pairing_discovery_payload_enabled", "true")
            _assign(tuning, "pairing_discovery_payload", f"0x{_parse_payload(payload):02X}")

    if CONF_PAIRING_DISCOVERY_LOW_POWER in config:
        _assign(
            tuning,
            "pairing_discovery_low_power",
            _cpp_bool(config[CONF_PAIRING_DISCOVERY_LOW_POWER]),
        )

    # --- Plain integer parameters: the C++ struct field name equals the YAML key. ---
    for key in _NUMBER_PARAMS:
        if key in config:
            _assign(tuning, key, config[key])

    cg.add(var.set_tuning_config(tuning))


async def _create_tuning_entities(config, var):
    """Generate number/select entities for the tuning parameters when enabled."""
    if not config[CONF_UI_CONTROLS]:
        return

    # --- Select entities (options sourced from _SELECT_OPTIONS) ---
    for key, options in _SELECT_OPTIONS.items():
        await _create_select(config, var, key, options)

    # --- Number entities (bounds sourced from _NUMBER_PARAMS) ---
    for key, (min_value, max_value, step, unit) in _NUMBER_PARAMS.items():
        await _create_number(
            config, var, key, min_value=min_value, max_value=max_value, step=step, unit=unit
        )


async def _create_number(
    config, var, key, min_value, max_value, step, unit=""
):
    """Create a single IOHomeTuningNumber entity.

    The bare {id, name} dict is normalized through number_schema()+COMPONENT_SCHEMA so it
    carries the entity/component defaults (disabled_by_default, setup_priority, ...) that
    register_number()/register_component() require.
    """
    schema = number.number_schema(
        IOHomeTuningNumber,
        unit_of_measurement=unit,
        entity_category=ENTITY_CATEGORY_CONFIG,
    )
    entity_config = schema.extend(cv.COMPONENT_SCHEMA)(
        {
            CONF_ID: config[_id_key(key)],
            CONF_NAME: UI_NAMES[key],
        }
    )
    entity = await number.new_number(
        entity_config,
        var,
        key,
        min_value=min_value,
        max_value=max_value,
        step=step,
    )
    await cg.register_component(entity, entity_config)


async def _create_select(config, var, key, options):
    """Create a single IOHomeTuningSelect entity.

    The bare {id, name} dict is normalized through select_schema()+COMPONENT_SCHEMA so it
    carries the entity/component defaults that register_select()/register_component() require.
    """
    entity_config = select.select_schema(
        IOHomeTuningSelect, entity_category=ENTITY_CATEGORY_CONFIG
    ).extend(cv.COMPONENT_SCHEMA)(
        {
            CONF_ID: config[_id_key(key)],
            CONF_NAME: UI_NAMES[key],
        }
    )
    entity = await select.new_select(
        entity_config,
        var,
        key,
        options=options,
    )
    await cg.register_component(entity, entity_config)


async def to_code(config, var):
    """Generate code for the tuning block."""
    _apply_tuning_config(config, var)
    await _create_tuning_entities(config, var)
