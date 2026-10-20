## @file
## @brief ESPHome fan platform schema and code generation (exterior heating).
## @ingroup hioc_codegen
##
## Defines the experimental IO-Homecontrol exterior-heating integration (a heat level as a fan speed)
## and wires it to the shared hub. Shared device-binding logic lives in platform_common.py; only
## fan-specific entity construction/registration stays here.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import fan
from esphome.const import CONF_ID

from . import home_io_control_ns
from .platform_common import (
    create_companion_sensors,
    inject_companion_sensor_ids,
    platform_schema_extension,
    wire_device_binding,
    CONF_HOME_IO_CONTROL_ID,
)

DEPENDENCIES = ["home_io_control"]

IOHomeFan = home_io_control_ns.class_("IOHomeFan", fan.Fan, cg.Component)


def _inject_companion_ids(config):
    return inject_companion_sensor_ids(config, CONF_ID)


# The entity never restores a saved state: a restored command would be sent to the heater at boot
# (the same trap the light platform guards against), and the first status poll is the truth.
CONFIG_SCHEMA = cv.All(
    fan.fan_schema(IOHomeFan, default_restore_mode="NO_RESTORE")
    .extend(platform_schema_extension())
    .extend(cv.COMPONENT_SCHEMA),
    _inject_companion_ids,
)


async def to_code(config):
    var = await fan.new_fan(config)
    await cg.register_component(var, config)

    parent = await cg.get_variable(config[CONF_HOME_IO_CONTROL_ID])
    await wire_device_binding(var, parent, config)
    await create_companion_sensors(config, parent)
