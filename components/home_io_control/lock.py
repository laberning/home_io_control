## @file
## @brief ESPHome lock platform schema and code generation.
## @ingroup hioc_codegen
##
## Defines the experimental IO-Homecontrol lock integration and wires it to the shared hub.
## Shared device-binding logic lives in platform_common.py; only lock-specific entity
## construction/registration stays here.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import lock
from esphome.const import CONF_ID

from . import home_io_control_ns
from .platform_common import (
    create_device_name_sensor,
    inject_device_name_sensor_id,
    platform_schema_extension,
    wire_device_binding,
    CONF_HOME_IO_CONTROL_ID,
)

DEPENDENCIES = ["home_io_control"]

IOHomeLock = home_io_control_ns.class_("IOHomeLock", lock.Lock, cg.Component)


def _inject_companion_ids(config):
    return inject_device_name_sensor_id(config, CONF_ID)


CONFIG_SCHEMA = cv.All(
    lock.lock_schema(IOHomeLock)
    .extend(platform_schema_extension())
    .extend(cv.COMPONENT_SCHEMA),
    _inject_companion_ids,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await lock.register_lock(var, config)

    parent = await cg.get_variable(config[CONF_HOME_IO_CONTROL_ID])
    await wire_device_binding(var, parent, config)
    await create_device_name_sensor(config, parent)
