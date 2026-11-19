## @file
## @brief ESPHome climate platform schema and code generation.
## @ingroup hioc_codegen
##
## Defines the experimental IO-Homecontrol heating/climate integration and wires it to the shared
## hub. A heater with a night light (io_model: heater_with_light) can add a dimmable `night_light:` light, which stays
## here: it is its own entity class, not a reading entity. Every other entity of a heater is a reading of
## DEVICE_READINGS (hub_validators.py), built by platform_common.py: the window sensor and the Open Window Detection
## switch of a heater with a window sensor (companions of their readings), and the opt-in keys: `heating_level:` on a
## plain heater or a towel dryer, `central_heating_mode:` on a towel dryer,
## `occupancy_detection:` and `occupancy:` on a radiator or a heater with a light, `energy:` on a radiator, a towel
## dryer or a heater with a light, and `remaining_time:`, `boost_duration:` and `drying_duration:` on a towel dryer.
## The heater is polled (default every five minutes); `status_poll_interval` sets the cadence. Shared device-binding
## logic lives in platform_common.py; only climate-specific entity construction/registration stays here.

import functools

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate, light
from esphome.const import (
    CONF_ID,
    CONF_MAX_TEMPERATURE,
    CONF_MIN_TEMPERATURE,
    CONF_OUTPUT_ID,
)

from . import home_io_control_ns
from .platform_common import (
    CLIMATE_PLATFORM,
    INSTANT_LIGHT_TRANSITION,
    declared_has_feature,
    create_companion_sensors,
    inherit_esphome_device,
    inject_companion_sensor_ids,
    inject_reading_companion_ids,
    platform_schema_extension,
    reading_companions_to_code,
    reading_keys_schema,
    reading_keys_to_code,
    validate_device_model,
    validate_reading_keys,
    validate_register_poll_interval,
    warn_if_experimental_kind,
    wire_device_binding,
    CONF_HOME_IO_CONTROL_ID,
    CONF_IO_DEVICE_ID,
)

DEPENDENCIES = ["home_io_control"]

IOHomeClimate = home_io_control_ns.class_("IOHomeClimate", climate.Climate, cg.Component)
IOHomeHeaterNightLight = home_io_control_ns.class_(
    "IOHomeHeaterNightLight", light.LightOutput, cg.Component
)

CONF_NIGHT_LIGHT = "night_light"


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


def _inject_companion_ids(config):
    """Declare the heater companion IDs during validation (ESPHome 2026.x StaticVector sizing): the reading
    companions of the declared kind (the window sensor and its detection switch, where it has WINDOW_COMPANIONS) and
    the diagnostic sensors."""
    inject_reading_companion_ids(config, CONF_ID, CLIMATE_PLATFORM)
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
            # The opt-in reading entities (the heating level, central-heating mode, occupancy, energy, run timers):
            # each but the level costs one register read per poll and only some heaters have the register or report
            # a level (DEVICE_READINGS in hub_validators.py).
            **reading_keys_schema(CLIMATE_PLATFORM),
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    functools.partial(validate_register_poll_interval, platform="climate"),
    _validate_temperature_range,
    validate_device_model,
    _validate_night_light,
    functools.partial(validate_reading_keys, platform=CLIMATE_PLATFORM),
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

    await reading_companions_to_code(config, parent, CLIMATE_PLATFORM)
    await reading_keys_to_code(var, parent, config, CLIMATE_PLATFORM)

    if CONF_NIGHT_LIGHT in config:
        night_light_config = config[CONF_NIGHT_LIGHT]
        inherit_esphome_device(night_light_config, config)
        night_light = cg.new_Pvariable(night_light_config[CONF_OUTPUT_ID])
        await cg.register_component(night_light, night_light_config)
        await light.register_light(night_light, night_light_config)
        cg.add(night_light.set_parent(parent))
        cg.add(night_light.set_device_id(config[CONF_IO_DEVICE_ID]))
