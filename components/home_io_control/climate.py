## @file
## @brief ESPHome climate platform schema and code generation.
## @ingroup hioc_codegen
##
## Defines the experimental IO-Homecontrol heating/climate integration and wires it to the shared
## hub. Shared device-binding logic lives in platform_common.py; only climate-specific entity
## construction/registration stays here.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate
from esphome.const import CONF_ID

from . import home_io_control_ns
from .platform_common import (
    create_companion_sensors,
    inject_companion_sensor_ids,
    platform_schema_extension,
    wire_device_binding,
    CONF_HOME_IO_CONTROL_ID,
    CONF_STATUS_POLL_INTERVAL,
)

DEPENDENCIES = ["home_io_control"]

IOHomeClimate = home_io_control_ns.class_("IOHomeClimate", climate.Climate, cg.Component)


def _inject_companion_ids(config):
    return inject_companion_sensor_ids(config, CONF_ID)


def _reject_status_poll_interval(config):
    # CMD_WRITE_PRIVATE (0x20) is write-only: a heating device has nothing to read back, so a
    # status poll interval is meaningless here (and would only schedule rejected polls against
    # the device). The key is accepted by the shared platform schema; reject it for climate.
    if CONF_STATUS_POLL_INTERVAL in config:
        raise cv.Invalid(
            "status_poll_interval is not supported for the climate platform: IO-Homecontrol "
            "heating is write-only and never polls",
            path=[CONF_STATUS_POLL_INTERVAL],
        )
    return config


CONFIG_SCHEMA = cv.All(
    climate.climate_schema(IOHomeClimate)
    .extend(platform_schema_extension())
    .extend(cv.COMPONENT_SCHEMA),
    _reject_status_poll_interval,
    _inject_companion_ids,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await climate.register_climate(var, config)

    parent = await cg.get_variable(config[CONF_HOME_IO_CONTROL_ID])
    await wire_device_binding(var, parent, config)
    await create_companion_sensors(config, parent)
