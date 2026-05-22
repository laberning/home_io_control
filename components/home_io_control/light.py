## @file
## @brief ESPHome binary light platform schema and code generation.
## @ingroup hioc_codegen
##
## Defines the experimental binary light integration and wires it to the shared hub.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light, text_sensor
from esphome.const import (
    CONF_DISABLED_BY_DEFAULT,
    CONF_ID,
    CONF_NAME,
    CONF_OUTPUT_ID,
    ENTITY_CATEGORY_DIAGNOSTIC,
)
from esphome.core import CORE, ID

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
CONF_LINKED_REMOTES = "linked_remotes"
CONF_DEVICE_TYPE = "io_device_type"
CONF_SUBTYPE = "io_subtype"
CONF_STATUS_POLL_INTERVAL = "status_poll_interval"

IOHomeLight = home_io_control_ns.class_("IOHomeLight", light.LightOutput, cg.Component)
IOHomeDeviceNameTextSensor = home_io_control_ns.class_(
    "IOHomeDeviceNameTextSensor", text_sensor.TextSensor, cg.Component
)


def device_name_sensor_id(parent_id):
    """Generate a unique ID for the diagnostic device-name text sensor."""
    return ID(
        f"{parent_id.id}_device_name_sensor",
        is_declaration=True,
        type=IOHomeDeviceNameTextSensor,
    )


def device_name_sensor_name(config):
    """Derive the device-name sensor name from the parent entity name."""
    base_name = config.get(CONF_NAME, "")
    if base_name:
        return f"{base_name} Device Name"
    return "Device Name"

CONFIG_SCHEMA = (
    # Expose this as a binary light on purpose. The transport may carry 0-100 values, but only
    # on/off semantics are backed by current protocol evidence, needs real device to verify
    light.light_schema(IOHomeLight, light.LightType.BINARY)
    .extend(
        {
            cv.GenerateID(CONF_HOME_IO_CONTROL_ID): cv.use_id(
                IOHomeControlComponent
            ),
            cv.Required(CONF_DEVICE_ID): validate_device_id,
            cv.Optional(CONF_DEVICE_TYPE): validate_device_type,
            cv.Optional(CONF_SUBTYPE): cv.int_range(min=0, max=63),
            cv.Optional(CONF_LINKED_REMOTES): cv.ensure_list(validate_device_id),
            cv.Optional(CONF_STATUS_POLL_INTERVAL): validate_status_poll_interval,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    await cg.register_component(var, config)
    await light.register_light(var, config)

    parent = await cg.get_variable(config[CONF_HOME_IO_CONTROL_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_device_id(config[CONF_DEVICE_ID]))

    if CONF_DEVICE_TYPE in config:
        cg.add(var.set_device_type(device_type_expression(config[CONF_DEVICE_TYPE])))
    if CONF_SUBTYPE in config:
        cg.add(var.set_subtype(config[CONF_SUBTYPE]))
    if CONF_STATUS_POLL_INTERVAL in config:
        cg.add(var.set_status_poll_interval(config[CONF_STATUS_POLL_INTERVAL].total_milliseconds))

    if CONF_LINKED_REMOTES in config:
        for remote_id in config[CONF_LINKED_REMOTES]:
            cg.add(parent.add_linked_remote(remote_id, config[CONF_DEVICE_ID]))

    device_name_config = {
        CONF_ID: device_name_sensor_id(config[CONF_OUTPUT_ID]),
        CONF_NAME: device_name_sensor_name(config),
        CONF_DISABLED_BY_DEFAULT: True,
        "entity_category": ENTITY_CATEGORY_DIAGNOSTIC,
    }
    device_name = await text_sensor.new_text_sensor(device_name_config)
    CORE.component_ids.add(str(device_name.base))
    await cg.register_component(device_name, device_name_config)
    cg.add(device_name.set_parent(parent))
    cg.add(device_name.set_device_id(config[CONF_DEVICE_ID]))