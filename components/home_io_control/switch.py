import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_ID

from . import (
    home_io_control_ns,
    IOHomeControlComponent,
    CONF_HOME_IO_CONTROL_ID,
    validate_device_id,
    validate_device_type,
    device_type_expression,
)

DEPENDENCIES = ["home_io_control"]

CONF_DEVICE_ID = "io_device_id"
CONF_LINKED_REMOTES = "linked_remotes"
CONF_DEVICE_TYPE = "io_device_type"
CONF_SUBTYPE = "io_subtype"

IOHomeSwitch = home_io_control_ns.class_("IOHomeSwitch", switch.Switch, cg.Component)

CONFIG_SCHEMA = (
    # Switches are exposed as binary entities; a richer state model needs real-device evidence
    # before it should be surfaced in the YAML schema.
    switch.switch_schema(IOHomeSwitch)
    .extend(
        {
            cv.GenerateID(CONF_HOME_IO_CONTROL_ID): cv.use_id(
                IOHomeControlComponent
            ),
            cv.Required(CONF_DEVICE_ID): validate_device_id,
            cv.Optional(CONF_DEVICE_TYPE): validate_device_type,
            cv.Optional(CONF_SUBTYPE): cv.int_range(min=0, max=63),
            cv.Optional(CONF_LINKED_REMOTES): cv.ensure_list(validate_device_id),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await switch.register_switch(var, config)

    parent = await cg.get_variable(config[CONF_HOME_IO_CONTROL_ID])
    # The runtime entity stays small: Python codegen only wires it to the shared controller and
    # provides the validated IO-homecontrol device ID.
    cg.add(var.set_parent(parent))
    cg.add(var.set_device_id(config[CONF_DEVICE_ID]))

    if CONF_DEVICE_TYPE in config:
        cg.add(var.set_device_type(device_type_expression(config[CONF_DEVICE_TYPE])))
    if CONF_SUBTYPE in config:
        cg.add(var.set_subtype(config[CONF_SUBTYPE]))

    if CONF_LINKED_REMOTES in config:
        for remote_id in config[CONF_LINKED_REMOTES]:
            cg.add(parent.add_linked_remote(remote_id, config[CONF_DEVICE_ID]))