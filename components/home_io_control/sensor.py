## @file
## @brief ESPHome sensor platform schema and code generation: the entry that declares a sensor-only device.
## @ingroup hioc_codegen
##
## A device that keeps its values in data objects (a heat pump's energy counters) has no cover, light or climate
## entry to declare it. This platform is its entry: `platform: home_io_control` with an `io_device_id:` and the
## reading keys its kind offers (the numeric readings of DEVICE_READINGS in hub_validators.py whose platform is `sensor`,
## through platform_common.reading_keys_schema()), read every `update_interval`. The entry's own sensor shows
## nothing and is marked internal; Home Assistant sees the readings and the companion diagnostics.
## Shared device-binding logic lives in platform_common.py; only sensor-specific construction stays here.

import functools

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_ID, CONF_INTERNAL, CONF_UPDATE_INTERVAL

from . import home_io_control_ns
from .hub_validators import inherit_device_into_nested, validate_device_reading_interval
from .platform_common import (
    SENSOR_PLATFORM,
    create_companion_sensors,
    inject_companion_sensor_ids,
    nested_companion_keys,
    platform_schema_extension,
    reading_keys_schema,
    reading_keys_to_code,
    validate_device_model,
    validate_reading_keys,
    warn_if_experimental_kind,
    wire_device_binding,
    CONF_HOME_IO_CONTROL_ID,
)

DEPENDENCIES = ["home_io_control"]

IOHomeDeviceSensor = home_io_control_ns.class_("IOHomeDeviceSensor", sensor.Sensor, cg.Component)


def _inject_companion_ids(config):
    return inject_companion_sensor_ids(config, CONF_ID)


def _hide_declaration_entity(config):
    """Mark the entry's own sensor internal: it only declares the device and never has a state to show."""
    config[CONF_INTERNAL] = True
    return config


def _config_schema():
    """The platform schema. A function so a test can build it after changing the reading mirror."""
    return cv.All(
        # First: the nested blocks need the sub-device before their schemas' duplicate-name check runs.
        inherit_device_into_nested(nested_companion_keys(SENSOR_PLATFORM)),
        sensor.sensor_schema(IOHomeDeviceSensor)
        .extend(platform_schema_extension())
        .extend(
            {
                # How often every reading of the entry is read; each reading has a default of its own.
                cv.Optional(CONF_UPDATE_INTERVAL): validate_device_reading_interval,
                **reading_keys_schema(SENSOR_PLATFORM),
            }
        )
        .extend(cv.COMPONENT_SCHEMA),
        validate_device_model,
        functools.partial(validate_reading_keys, platform=SENSOR_PLATFORM),
        _hide_declaration_entity,
        warn_if_experimental_kind,
        _inject_companion_ids,
    )


CONFIG_SCHEMA = _config_schema()


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await sensor.register_sensor(var, config)

    parent = await cg.get_variable(config[CONF_HOME_IO_CONTROL_ID])
    await wire_device_binding(var, parent, config)
    await create_companion_sensors(config, parent)

    # An update_interval: key reaches the entity as set_update_interval(), which register_component() emits.
    await reading_keys_to_code(var, parent, config, SENSOR_PLATFORM)
