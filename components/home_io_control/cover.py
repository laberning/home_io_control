## @file
## @brief ESPHome cover platform schema and code generation.
## @ingroup hioc_codegen
##
## Bridges the YAML ``cover:`` platform declaration to the runtime IOHomeCover entity.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import cover
from esphome.const import CONF_ID

from . import (
    home_io_control_ns,
    IOHomeControlComponent,
    CONF_HOME_IO_CONTROL_ID,
    validate_device_id,
    validate_status_poll_interval,
    validate_device_type,
    device_type_expression,
)

DEPENDENCIES = ["home_io_control"]

CONF_DEVICE_ID = "io_device_id"
CONF_INVERT_POSITION = "invert_position"
CONF_LINKED_REMOTES = "linked_remotes"
CONF_DEVICE_TYPE = "io_device_type"
CONF_SUBTYPE = "io_subtype"
CONF_STATUS_POLL_INTERVAL = "status_poll_interval"

IOHomeCover = home_io_control_ns.class_("IOHomeCover", cover.Cover, cg.Component)

CONFIG_SCHEMA = (
    cover.cover_schema(IOHomeCover)
    .extend(
        {
            cv.GenerateID(CONF_HOME_IO_CONTROL_ID): cv.use_id(
                IOHomeControlComponent
            ),
            cv.Required(CONF_DEVICE_ID): validate_device_id,
            cv.Optional(CONF_INVERT_POSITION): cv.boolean,
            cv.Optional(CONF_DEVICE_TYPE): validate_device_type,
            cv.Optional(CONF_SUBTYPE): cv.int_range(min=0, max=63),
            cv.Optional(CONF_LINKED_REMOTES): cv.ensure_list(validate_device_id),
            cv.Optional(CONF_STATUS_POLL_INTERVAL): validate_status_poll_interval,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await cover.new_cover(config)
    await cg.register_component(var, config)

    parent = await cg.get_variable(config[CONF_HOME_IO_CONTROL_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_device_id(config[CONF_DEVICE_ID]))

    if CONF_INVERT_POSITION in config:
        cg.add(var.set_invert_position(config[CONF_INVERT_POSITION]))

    if CONF_DEVICE_TYPE in config:
        cg.add(var.set_device_type(device_type_expression(config[CONF_DEVICE_TYPE])))
    if CONF_SUBTYPE in config:
        cg.add(var.set_subtype(config[CONF_SUBTYPE]))
    if CONF_STATUS_POLL_INTERVAL in config:
        cg.add(var.set_status_poll_interval(config[CONF_STATUS_POLL_INTERVAL].total_milliseconds))

    if CONF_LINKED_REMOTES in config:
        for remote_id in config[CONF_LINKED_REMOTES]:
            cg.add(parent.add_linked_remote(remote_id, config[CONF_DEVICE_ID]))
