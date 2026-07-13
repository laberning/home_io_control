## @file
## @brief ESPHome binary light platform schema and code generation.
## @ingroup hioc_codegen
##
## Defines the experimental binary light integration and wires it to the shared hub.
## Shared device-binding logic lives in platform_common.py; only light-specific entity
## construction/registration stays here.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light
from esphome.const import CONF_OUTPUT_ID

from . import home_io_control_ns
from .platform_common import (
    create_device_name_sensor,
    create_last_result_sensor,
    inject_device_name_sensor_id,
    inject_last_result_sensor_id,
    platform_schema_extension,
    wire_device_binding,
    CONF_HOME_IO_CONTROL_ID,
)

DEPENDENCIES = ["home_io_control"]

IOHomeLight = home_io_control_ns.class_("IOHomeLight", light.LightOutput, cg.Component)


def _inject_companion_ids(config):
    # Light reads its entity ID from CONF_OUTPUT_ID rather than CONF_ID.
    inject_device_name_sensor_id(config, CONF_OUTPUT_ID)
    return inject_last_result_sensor_id(config, CONF_OUTPUT_ID)


CONFIG_SCHEMA = cv.All(
    # Expose this as a binary light on purpose. The transport may carry 0-100 values, but only
    # on/off semantics are backed by current protocol evidence, needs real device to verify
    light.light_schema(IOHomeLight, light.LightType.BINARY)
    .extend(platform_schema_extension())
    .extend(cv.COMPONENT_SCHEMA),
    _inject_companion_ids,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    await cg.register_component(var, config)
    await light.register_light(var, config)

    parent = await cg.get_variable(config[CONF_HOME_IO_CONTROL_ID])
    await wire_device_binding(var, parent, config)
    await create_device_name_sensor(config, parent)
    await create_last_result_sensor(config, parent)
