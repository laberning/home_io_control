## @file
## @brief ESPHome binary switch platform schema and code generation.
## @ingroup hioc_codegen
##
## Defines the experimental on/off switch integration and wires it to the shared hub.
## Shared device-binding logic lives in platform_common.py; only switch-specific entity
## construction/registration stays here. The hub-level "Accept Foreign Pairing (Key Extraction)"
## switch is unrelated to this platform — it has no device to bind to, so it is not exposed
## through `switch:` at all; see `accept_foreign_pairing` in `__init__.py`.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
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

IOHomeSwitch = home_io_control_ns.class_("IOHomeSwitch", switch.Switch, cg.Component)


def _inject_companion_ids(config):
    return inject_companion_sensor_ids(config, CONF_ID)


CONFIG_SCHEMA = cv.All(
    # Switches are exposed as binary entities; a richer state model needs real-device evidence
    # before it should be surfaced in the YAML schema.
    switch.switch_schema(IOHomeSwitch)
    .extend(platform_schema_extension())
    .extend(cv.COMPONENT_SCHEMA),
    _inject_companion_ids,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await switch.register_switch(var, config)

    parent = await cg.get_variable(config[CONF_HOME_IO_CONTROL_ID])
    # The runtime entity stays small: Python codegen only wires it to the shared controller and
    # provides the validated IO-homecontrol device ID.
    await wire_device_binding(var, parent, config)
    await create_companion_sensors(config, parent)
