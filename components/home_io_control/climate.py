## @file
## @brief ESPHome climate platform schema and code generation.
## @ingroup hioc_codegen
##
## Defines the experimental IO-Homecontrol heating/climate integration and wires it to the shared
## hub. Heaters with a window sensor also get a Window binary sensor and an Open Window Detection
## switch; a heater with a night light (io_model: heater_with_light) can add a dimmable `night_light:`
## light, an electrical heater can add a `heating_level:` text sensor with the level it applies now, and a towel
## dryer (io_model: towel_dryer) can add a `central_heating_mode:` binary sensor. The heater is polled (default every five minutes); `status_poll_interval` sets the cadence. Shared device-binding logic lives in platform_common.py; only climate-specific entity
## construction/registration stays here.

import functools

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, climate, light, switch, text_sensor
from esphome.const import (
    CONF_DISABLED_BY_DEFAULT,
    CONF_ID,
    CONF_MAX_TEMPERATURE,
    CONF_MIN_TEMPERATURE,
    CONF_OUTPUT_ID,
    DEVICE_CLASS_WINDOW,
    ENTITY_CATEGORY_CONFIG,
)

from . import home_io_control_ns
from .hub_validators import (
    DEVICE_TYPE_OPTIONS,
    FEATURE_CENTRAL_HEATING_MODE,
    FEATURE_NIGHT_LIGHT,
    FEATURE_TEMPERATURE_RANGE,
    FEATURE_WINDOW_COMPANIONS,
)
from .platform_common import (
    INSTANT_LIGHT_TRANSITION,
    declared_features,
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
IOHomeOpenWindowDetectionSwitch = home_io_control_ns.class_(
    "IOHomeOpenWindowDetectionSwitch", switch.Switch, cg.Component
)
IOHomeHeaterNightLight = home_io_control_ns.class_(
    "IOHomeHeaterNightLight", light.LightOutput, cg.Component
)

IOHomeHeatingLevelSensor = home_io_control_ns.class_(
    "IOHomeHeatingLevelSensor", text_sensor.TextSensor, cg.Component
)

IOHomeCentralHeatingModeBinarySensor = home_io_control_ns.class_(
    "IOHomeCentralHeatingModeBinarySensor", binary_sensor.BinarySensor, cg.Component
)

CONF_NIGHT_LIGHT = "night_light"
CONF_CENTRAL_HEATING_MODE = "central_heating_mode"
CONF_HEATING_LEVEL = "heating_level"

# Internal config keys for the heater-only companion IDs (injected by the post-validator).
CONF_WINDOW_SENSOR_ID = "_window_sensor_id"
CONF_WINDOW_DETECTION_SWITCH_ID = "_window_detection_switch_id"


def _validate_night_light(config):
    """Accept `night_light:` only on a heater kind that has one (the NIGHT_LIGHT bit of its row).

    Unlike require_feature(), this also rejects the key when no io_device_type is declared: the light's
    registers would otherwise go to a heater nothing says has them.
    """
    if CONF_NIGHT_LIGHT not in config or declared_features(config) & FEATURE_NIGHT_LIGHT:
        return config
    raise cv.Invalid(
        "night_light needs a heater with a night light: set io_device_type: electrical_heater, "
        "io_subtype: 1 and io_model: heater_with_light",
        path=[CONF_NIGHT_LIGHT],
    )


def _validate_central_heating_mode(config):
    """Accept `central_heating_mode:` only on a kind with the CENTRAL_HEATING_MODE bit (a towel dryer).

    Like `night_light:`, the key is rejected when no io_device_type is declared: the register would
    otherwise be read from a heater nothing says has it.
    """
    if CONF_CENTRAL_HEATING_MODE not in config or declared_features(config) & FEATURE_CENTRAL_HEATING_MODE:
        return config
    raise cv.Invalid(
        "central_heating_mode needs a towel dryer: set io_device_type: electrical_heater, "
        "io_subtype: 1 and io_model: towel_dryer",
        path=[CONF_CENTRAL_HEATING_MODE],
    )


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
# switch exist when the declared kind's row has the WINDOW_COMPANIONS bit, so a towel dryer (a model row
# without it) gets none. A heater declared without a type gets none: nothing at validation time says what
# it is.
CLIMATE_COMPANIONS = (
    (CONF_WINDOW_SENSOR_ID, "window_sensor", IOHomeWindowBinarySensor, has_feature(FEATURE_WINDOW_COMPANIONS)),
    (
        CONF_WINDOW_DETECTION_SWITCH_ID,
        "window_detection_switch",
        IOHomeOpenWindowDetectionSwitch,
        has_feature(FEATURE_WINDOW_COMPANIONS),
    ),
)


def _inject_companion_ids(config):
    """Declare the heater companion IDs during validation (ESPHome 2026.x StaticVector sizing)."""
    inject_companion_ids(config, CLIMATE_COMPANIONS, CONF_ID)
    return inject_companion_sensor_ids(config, CONF_ID)


# The range a heating temperature interface encodes its temperature over. It defaults to -40..+80 degrees
# Celsius (proto_heating.h) as an assumption; a device that turns out to use another range is told here.
# A heat pump uses the fixed range, so the keys do not apply to it (the TEMPERATURE_RANGE bit). Mirrors
# HEATING_TEMPERATURE_RANGE_MIN_C / _MAX_C.
DEFAULT_TEMPERATURE_RANGE_C = (-40.0, 80.0)
TEMPERATURE_RANGE_BOUNDS_C = (-50.0, 100.0)


def _validate_temperature_range(config):
    # Applies to the two keys only when one is given; the other keeps its default.
    if CONF_MIN_TEMPERATURE not in config and CONF_MAX_TEMPERATURE not in config:
        return config
    # The TEMPERATURE_RANGE bit of the declared kind; no declared kind has no bit, so the keys stay rejected
    # there as well (the range would otherwise go to a device nothing says has one).
    if not declared_features(config) & FEATURE_TEMPERATURE_RANGE:
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
            # On while a mixed towel dryer runs on central heating (its element stays off); opt-in, since
            # it costs one register read per poll and only a mixed unit has the answer. Its ID is declared
            # by the schema's own GenerateID.
            cv.Optional(CONF_CENTRAL_HEATING_MODE): binary_sensor.binary_sensor_schema(
                IOHomeCentralHeatingModeBinarySensor
            ).extend(cv.COMPONENT_SCHEMA),
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    functools.partial(validate_register_poll_interval, platform="climate"),
    _validate_temperature_range,
    validate_device_model,
    _validate_night_light,
    _validate_heating_level,
    _validate_central_heating_mode,
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
        await create_companion(
            config,
            parent,
            schema=switch.switch_schema(
                IOHomeOpenWindowDetectionSwitch,
                default_restore_mode="DISABLED",
                entity_category=ENTITY_CATEGORY_CONFIG,
            ),
            new_entity=switch.new_switch,
            id_key=CONF_WINDOW_DETECTION_SWITCH_ID,
            name=companion_entity_name(config, "Open Window Detection"),
            overrides={CONF_DISABLED_BY_DEFAULT: True},
        )

    if CONF_HEATING_LEVEL in config:
        await create_user_companion(
            config,
            parent,
            new_entity=text_sensor.new_text_sensor,
            entity_config=config[CONF_HEATING_LEVEL],
        )

    if CONF_CENTRAL_HEATING_MODE in config:
        cg.add(var.set_reads_central_heating_mode())
        await create_user_companion(
            config,
            parent,
            new_entity=binary_sensor.new_binary_sensor,
            entity_config=config[CONF_CENTRAL_HEATING_MODE],
        )

    if CONF_NIGHT_LIGHT in config:
        night_light_config = config[CONF_NIGHT_LIGHT]
        inherit_esphome_device(night_light_config, config)
        night_light = cg.new_Pvariable(night_light_config[CONF_OUTPUT_ID])
        await cg.register_component(night_light, night_light_config)
        await light.register_light(night_light, night_light_config)
        cg.add(night_light.set_parent(parent))
        cg.add(night_light.set_device_id(config[CONF_IO_DEVICE_ID]))
