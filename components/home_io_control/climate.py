## @file
## @brief ESPHome climate platform schema and code generation.
## @ingroup hioc_codegen
##
## Defines the experimental IO-Homecontrol heating/climate integration and wires it to the shared
## hub. Heaters with a window sensor also get a Window binary sensor and an Open Window Detection
## switch; a heater with a night light (io_model: heater_with_light) can add a dimmable `night_light:`
## light, an electrical heater can add a `heating_level:` text sensor with the level it applies now, a towel
## dryer (io_model: towel_dryer) can add a `central_heating_mode:` binary sensor, and a radiator or a heater with a
## light can add an `occupancy_detection:` switch and an `occupancy:` binary sensor, and a radiator, a towel dryer
## or a heater with a light can add an `energy:` sensor with the energy total the heater counts, and a towel dryer can
## add `remaining_time:`, `boost_duration:` and `drying_duration:` sensors with its boost and drying run times.
## The heater is polled (default every five minutes); `status_poll_interval` sets the cadence. Shared device-binding logic lives in platform_common.py;
## only climate-specific entity construction/registration stays here.

import functools
from typing import NamedTuple

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, climate, light, sensor, switch, text_sensor
from esphome.const import (
    CONF_DISABLED_BY_DEFAULT,
    CONF_ID,
    CONF_MAX_TEMPERATURE,
    CONF_MIN_TEMPERATURE,
    CONF_OUTPUT_ID,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_OCCUPANCY,
    DEVICE_CLASS_WINDOW,
    ENTITY_CATEGORY_CONFIG,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_KILOWATT_HOURS,
    UNIT_MINUTE,
)

from . import home_io_control_ns
from .hub_validators import (
    DEVICE_TYPE_OPTIONS,
)
from .platform_common import (
    INSTANT_LIGHT_TRANSITION,
    declared_has_feature,
    companion_entity_name,
    create_companion,
    create_companion_sensors,
    create_user_companion,
    has_feature,
    inherit_esphome_device,
    inject_companion_ids,
    inject_companion_sensor_ids,
    platform_schema_extension,
    validate_device_model,
    validate_register_poll_interval,
    warn_if_experimental_kind,
    wire_device_binding,
    CONF_DEVICE_TYPE,
    CONF_HOME_IO_CONTROL_ID,
    CONF_IO_DEVICE_ID,
)

DEPENDENCIES = ["home_io_control"]

IOHomeClimate = home_io_control_ns.class_("IOHomeClimate", climate.Climate, cg.Component)
IOHomeWindowBinarySensor = home_io_control_ns.class_(
    "IOHomeWindowBinarySensor", binary_sensor.BinarySensor, cg.Component
)
IOHomeHeaterSettingSwitch = home_io_control_ns.class_(
    "IOHomeHeaterSettingSwitch", switch.Switch, cg.Component
)
IOHomeHeaterNightLight = home_io_control_ns.class_(
    "IOHomeHeaterNightLight", light.LightOutput, cg.Component
)

IOHomeHeatingLevelSensor = home_io_control_ns.class_(
    "IOHomeHeatingLevelSensor", text_sensor.TextSensor, cg.Component
)

IOHomeHeaterRegisterBinarySensor = home_io_control_ns.class_(
    "IOHomeHeaterRegisterBinarySensor", binary_sensor.BinarySensor, cg.Component
)

IOHomeHeaterRegisterSensor = home_io_control_ns.class_(
    "IOHomeHeaterRegisterSensor", sensor.Sensor, cg.Component
)

# The registers a heater entity is named after (proto_heating.h HeatingFunction).
HeatingFunction = home_io_control_ns.enum("HeatingFunction", is_class=True)

CONF_NIGHT_LIGHT = "night_light"
CONF_CENTRAL_HEATING_MODE = "central_heating_mode"
CONF_OCCUPANCY_DETECTION = "occupancy_detection"
CONF_OCCUPANCY = "occupancy"
CONF_ENERGY = "energy"
CONF_REMAINING_TIME = "remaining_time"
CONF_BOOST_DURATION = "boost_duration"
CONF_DRYING_DURATION = "drying_duration"
CONF_HEATING_LEVEL = "heating_level"

# Internal config keys for the heater-only companion IDs (injected by the post-validator).
CONF_WINDOW_SENSOR_ID = "_window_sensor_id"
CONF_WINDOW_DETECTION_SWITCH_ID = "_window_detection_switch_id"


def _validate_night_light(config):
    """Accept `night_light:` only on a heater kind that has one (the NIGHT_LIGHT capability of its row).

    Unlike require_feature(), this also rejects the key when no io_device_type is declared: the light's
    registers would otherwise go to a heater nothing says has them.
    """
    if CONF_NIGHT_LIGHT not in config or declared_has_feature(config, "NIGHT_LIGHT"):
        return config
    raise cv.Invalid(
        "night_light needs a heater with a night light: set io_device_type: electrical_heater, "
        "io_subtype: 1 and io_model: heater_with_light",
        path=[CONF_NIGHT_LIGHT],
    )


class OptInRead(NamedTuple):
    """A climate key whose entity shows heater registers that a poll reads only where the key is declared.

    `functions` are the HeatingFunctions the poll reads for the key (one register, or the registers the entity
    chooses between), `feature` the capability a kind needs for the key (OPT_IN_READ_FEATURES, by the register;
    the registers of one key share it), `hint` says which kind that is, and `schema` / `new_entity` build the
    user's entity. An entity that shows one register is told which; `shows_timed_run_remaining` marks the one
    that shows the minutes left of whichever timed run is active instead.
    """

    key: str
    hint: str
    functions: tuple
    schema: object
    new_entity: object
    shows_timed_run_remaining: bool = False

    @property
    def feature(self):
        """The capability a kind needs for this key: the one that gates the hub's read of its registers."""
        (feature,) = {OPT_IN_READ_FEATURES[function] for function in self.functions}
        return feature


# The capability each opt-in register needs, by HeatingFunction name: the mirror of HEATING_OPT_IN_READS
# (proto_heating_profiles.cpp), which gates the hub's read; tests/sync/device_features_sync_test.cpp checks it.
OPT_IN_READ_FEATURES = {
    "CENTRAL_HEATING_MODE": "CENTRAL_HEATING_MODE",
    "OCCUPANCY_DETECTION": "OCCUPANCY",
    "SET_PRESENCE": "OCCUPANCY",
    "ENERGY_TOTAL": "ENERGY_METER",
    "BOOST_REMAINING": "RUN_TIMERS",
    "BOOST_DURATION": "RUN_TIMERS",
    "DRYING_REMAINING": "RUN_TIMERS",
    "DRYING_DURATION": "RUN_TIMERS",
}


_OCCUPANCY_HINT = (
    "a heater with occupancy detection: set io_device_type: electrical_heater, io_subtype: 1 and either "
    "no io_model (a radiator) or io_model: heater_with_light"
)

_ENERGY_HINT = (
    "a heater that counts its energy: set io_device_type: electrical_heater, io_subtype: 1 and either no "
    "io_model (a radiator), io_model: towel_dryer or io_model: heater_with_light"
)

_RUN_TIMERS_HINT = "a towel dryer: set io_device_type: electrical_heater, io_subtype: 1 and io_model: towel_dryer"

# Opt-in entities show a register the heater may not have and cost one read per poll, so each is a key of the
# climate entry; its read, its capability check and its codegen all come from this table.
OPT_IN_READS = (
    # On while a mixed towel dryer runs on central heating (its element stays off). Its ID is declared by the
    # schema's own GenerateID.
    OptInRead(
        CONF_CENTRAL_HEATING_MODE,
        "a towel dryer: set io_device_type: electrical_heater, io_subtype: 1 and io_model: towel_dryer",
        ("CENTRAL_HEATING_MODE",),
        binary_sensor.binary_sensor_schema(IOHomeHeaterRegisterBinarySensor).extend(cv.COMPONENT_SCHEMA),
        binary_sensor.new_binary_sensor,
    ),
    # The heater's occupancy detection setting. Restore mode DISABLED: the switch shows what the heater
    # reports, never a restored guess.
    OptInRead(
        CONF_OCCUPANCY_DETECTION,
        _OCCUPANCY_HINT,
        ("OCCUPANCY_DETECTION",),
        switch.switch_schema(
            IOHomeHeaterSettingSwitch,
            default_restore_mode="DISABLED",
            entity_category=ENTITY_CATEGORY_CONFIG,
        ).extend(cv.COMPONENT_SCHEMA),
        switch.new_switch,
    ),
    # On while the heater sees someone in the room.
    OptInRead(
        CONF_OCCUPANCY,
        _OCCUPANCY_HINT,
        ("SET_PRESENCE",),
        binary_sensor.binary_sensor_schema(
            IOHomeHeaterRegisterBinarySensor, device_class=DEVICE_CLASS_OCCUPANCY
        ).extend(cv.COMPONENT_SCHEMA),
        binary_sensor.new_binary_sensor,
    ),
    # The energy the heater has used, whole kWh as its display shows it. A numeric register entity: another one
    # is a row here with its own unit, device class and state class.
    OptInRead(
        CONF_ENERGY,
        _ENERGY_HINT,
        ("ENERGY_TOTAL",),
        sensor.sensor_schema(
            IOHomeHeaterRegisterSensor,
            unit_of_measurement=UNIT_KILOWATT_HOURS,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_ENERGY,
            state_class=STATE_CLASS_TOTAL_INCREASING,
        ).extend(cv.COMPONENT_SCHEMA),
        sensor.new_sensor,
    ),
    # The minutes left of the boost or drying run the dryer is in, 0 with none: two registers, one read per poll
    # while a run is on, chosen by the dryer's temporary state.
    OptInRead(
        CONF_REMAINING_TIME,
        _RUN_TIMERS_HINT,
        ("BOOST_REMAINING", "DRYING_REMAINING"),
        sensor.sensor_schema(
            IOHomeHeaterRegisterSensor,
            unit_of_measurement=UNIT_MINUTE,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_DURATION,
            state_class=STATE_CLASS_MEASUREMENT,
        ).extend(cv.COMPONENT_SCHEMA),
        sensor.new_sensor,
        shows_timed_run_remaining=True,
    ),
    # The boost duration set on the dryer. Two bytes on the wire (the duration and the panel's maximum); the
    # sensor shows the duration.
    OptInRead(
        CONF_BOOST_DURATION,
        _RUN_TIMERS_HINT,
        ("BOOST_DURATION",),
        sensor.sensor_schema(
            IOHomeHeaterRegisterSensor,
            unit_of_measurement=UNIT_MINUTE,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_DURATION,
        ).extend(cv.COMPONENT_SCHEMA),
        sensor.new_sensor,
    ),
    # The duration of the last drying run the dryer started: a change on the panel alone shows at the next run.
    OptInRead(
        CONF_DRYING_DURATION,
        _RUN_TIMERS_HINT,
        ("DRYING_DURATION",),
        sensor.sensor_schema(
            IOHomeHeaterRegisterSensor,
            unit_of_measurement=UNIT_MINUTE,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_DURATION,
        ).extend(cv.COMPONENT_SCHEMA),
        sensor.new_sensor,
    ),
)


def _validate_opt_in_reads(config):
    """Accept each opt-in key (OPT_IN_READS) only on a kind with the capability of its register.

    Like `night_light:`, a key is rejected when no io_device_type is declared: the register would otherwise
    be read from a heater nothing says has it.
    """
    for read in OPT_IN_READS:
        if read.key in config and not declared_has_feature(config, read.feature):
            raise cv.Invalid(f"{read.key} needs {read.hint}", path=[read.key])
    return config


def _validate_heating_level(config):
    """Accept `heating_level:` only on an electrical heater.

    Every electrical heater accepts it: the sensor stays without a state where the heater reports no level
    (a pilot-wire interface, a radiator), which the docs say. Like `night_light:`, the key is rejected when
    no io_device_type is declared: nothing at validation time says the device is a heater.
    """
    if CONF_HEATING_LEVEL not in config or config.get(CONF_DEVICE_TYPE) == DEVICE_TYPE_OPTIONS["electrical_heater"]:
        return config
    raise cv.Invalid(
        "heating_level needs an electrical heater: set io_device_type: electrical_heater",
        path=[CONF_HEATING_LEVEL],
    )


# The heater companions: (id key, ID suffix, class, condition). The window sensor and its detection
# switch exist when the declared kind's row has the WINDOW_COMPANIONS capability, so a towel dryer (a model row
# without it) gets none. A heater declared without a type gets none: nothing at validation time says what
# it is.
CLIMATE_COMPANIONS = (
    (CONF_WINDOW_SENSOR_ID, "window_sensor", IOHomeWindowBinarySensor, has_feature("WINDOW_COMPANIONS")),
    (
        CONF_WINDOW_DETECTION_SWITCH_ID,
        "window_detection_switch",
        IOHomeHeaterSettingSwitch,
        has_feature("WINDOW_COMPANIONS"),
    ),
)


def _inject_companion_ids(config):
    """Declare the heater companion IDs during validation (ESPHome 2026.x StaticVector sizing)."""
    inject_companion_ids(config, CLIMATE_COMPANIONS, CONF_ID)
    return inject_companion_sensor_ids(config, CONF_ID)


# The range a heating temperature interface encodes its temperature over. It defaults to -40..+80 degrees
# Celsius (proto_heating.h) as an assumption; a device that turns out to use another range is told here.
# A heat pump uses the fixed range, so the keys do not apply to it (the TEMPERATURE_RANGE capability). Mirrors
# HEATING_TEMPERATURE_RANGE_MIN_C / _MAX_C.
DEFAULT_TEMPERATURE_RANGE_C = (-40.0, 80.0)
TEMPERATURE_RANGE_BOUNDS_C = (-50.0, 100.0)


def _validate_temperature_range(config):
    # Applies to the two keys only when one is given; the other keeps its default.
    if CONF_MIN_TEMPERATURE not in config and CONF_MAX_TEMPERATURE not in config:
        return config
    # The TEMPERATURE_RANGE capability of the declared kind; no declared kind has none, so the keys stay rejected
    # there as well (the range would otherwise go to a device nothing says has one).
    if not declared_has_feature(config, "TEMPERATURE_RANGE"):
        raise cv.Invalid(
            "min_temperature and max_temperature apply to io_device_type: heating_temperature_interface "
            "only: a heat pump uses a fixed range, and every other device has no temperature range"
        )
    low, high = TEMPERATURE_RANGE_BOUNDS_C
    for key in (CONF_MIN_TEMPERATURE, CONF_MAX_TEMPERATURE):
        if key in config and not low <= config[key] <= high:
            raise cv.Invalid(
                f"{key} must be between {low:g} and {high:g} degrees Celsius",
                path=[key],
            )
    minimum = config.get(CONF_MIN_TEMPERATURE, DEFAULT_TEMPERATURE_RANGE_C[0])
    maximum = config.get(CONF_MAX_TEMPERATURE, DEFAULT_TEMPERATURE_RANGE_C[1])
    if minimum >= maximum:
        raise cv.Invalid(
            f"min_temperature ({minimum:g}) must be below max_temperature ({maximum:g}): they are the "
            "range the device encodes its temperature over, not the range of the card"
        )
    return config


CONFIG_SCHEMA = cv.All(
    climate.climate_schema(IOHomeClimate)
    .extend(platform_schema_extension())
    .extend(
        {
            cv.Optional(CONF_MIN_TEMPERATURE): cv.temperature,
            cv.Optional(CONF_MAX_TEMPERATURE): cv.temperature,
            # A dimmable light; its IDs are declared by the schema's own GenerateIDs.
            cv.Optional(CONF_NIGHT_LIGHT): light.light_schema(
                IOHomeHeaterNightLight, light.LightType.BRIGHTNESS_ONLY
            )
            .extend(INSTANT_LIGHT_TRANSITION)
            .extend(cv.COMPONENT_SCHEMA),
            # The level the heater applies now (comfort, eco, ...); its ID is declared by the schema's own
            # GenerateID.
            cv.Optional(CONF_HEATING_LEVEL): text_sensor.text_sensor_schema(IOHomeHeatingLevelSensor).extend(
                cv.COMPONENT_SCHEMA
            ),
            # The opt-in register entities (central-heating mode, occupancy, energy, run timers): each costs one register
            # read per poll and only some heaters have the register. Their IDs are declared by the schemas' own
            # GenerateID.
            **{cv.Optional(read.key): read.schema for read in OPT_IN_READS},
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    functools.partial(validate_register_poll_interval, platform="climate"),
    _validate_temperature_range,
    validate_device_model,
    _validate_night_light,
    _validate_heating_level,
    _validate_opt_in_reads,
    warn_if_experimental_kind,
    _inject_companion_ids,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await climate.register_climate(var, config)

    parent = await cg.get_variable(config[CONF_HOME_IO_CONTROL_ID])
    await wire_device_binding(var, parent, config)
    await create_companion_sensors(config, parent)

    if CONF_MIN_TEMPERATURE in config or CONF_MAX_TEMPERATURE in config:
        cg.add(
            var.set_temperature_range(
                config.get(CONF_MIN_TEMPERATURE, DEFAULT_TEMPERATURE_RANGE_C[0]),
                config.get(CONF_MAX_TEMPERATURE, DEFAULT_TEMPERATURE_RANGE_C[1]),
            )
        )

    if CONF_WINDOW_SENSOR_ID in config:
        await create_companion(
            config,
            parent,
            schema=binary_sensor.binary_sensor_schema(
                IOHomeWindowBinarySensor, device_class=DEVICE_CLASS_WINDOW
            ),
            new_entity=binary_sensor.new_binary_sensor,
            id_key=CONF_WINDOW_SENSOR_ID,
            name=companion_entity_name(config, "Window"),
        )

    if CONF_WINDOW_DETECTION_SWITCH_ID in config:
        # Restore mode DISABLED: the switch shows what the heater reports, never a restored guess.
        detection = await create_companion(
            config,
            parent,
            schema=switch.switch_schema(
                IOHomeHeaterSettingSwitch,
                default_restore_mode="DISABLED",
                entity_category=ENTITY_CATEGORY_CONFIG,
            ),
            new_entity=switch.new_switch,
            id_key=CONF_WINDOW_DETECTION_SWITCH_ID,
            name=companion_entity_name(config, "Open Window Detection"),
            overrides={CONF_DISABLED_BY_DEFAULT: True},
        )
        cg.add(detection.set_function(HeatingFunction.SET_OPEN_WINDOW_DETECTION))

    if CONF_HEATING_LEVEL in config:
        await create_user_companion(
            config,
            parent,
            new_entity=text_sensor.new_text_sensor,
            entity_config=config[CONF_HEATING_LEVEL],
        )

    for read in OPT_IN_READS:
        if read.key not in config:
            continue
        functions = [getattr(HeatingFunction, name) for name in read.functions]
        for function in functions:
            cg.add(var.add_register_read(function))
        entity = await create_user_companion(
            config,
            parent,
            new_entity=read.new_entity,
            entity_config=config[read.key],
        )
        if read.shows_timed_run_remaining:
            cg.add(entity.set_shows_timed_run_remaining())
        else:
            cg.add(entity.set_function(functions[0]))

    if CONF_NIGHT_LIGHT in config:
        night_light_config = config[CONF_NIGHT_LIGHT]
        inherit_esphome_device(night_light_config, config)
        night_light = cg.new_Pvariable(night_light_config[CONF_OUTPUT_ID])
        await cg.register_component(night_light, night_light_config)
        await light.register_light(night_light, night_light_config)
        cg.add(night_light.set_parent(parent))
        cg.add(night_light.set_device_id(config[CONF_IO_DEVICE_ID]))
