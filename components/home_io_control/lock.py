## @file
## @brief ESPHome lock platform schema and code generation.
## @ingroup hioc_codegen
##
## Defines the experimental IO-Homecontrol lock integration and wires it to the shared hub.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import lock, text_sensor
from esphome.const import (
    CONF_DISABLED_BY_DEFAULT,
    CONF_ID,
    CONF_NAME,
    ENTITY_CATEGORY_DIAGNOSTIC,
)
from esphome.core import ID

from . import (
    home_io_control_ns,
    IOHomeControlComponent,
    CONF_HOME_IO_CONTROL_ID,
    device_type_expression,
    validate_device_id,
    validate_device_type,
    validate_status_poll_interval,
)

DEPENDENCIES = ["home_io_control"]

CONF_DEVICE_ID = "io_device_id"
CONF_LINKED_REMOTES = "linked_remotes"
CONF_DEVICE_TYPE = "io_device_type"
CONF_SUBTYPE = "io_subtype"
CONF_STATUS_POLL_INTERVAL = "status_poll_interval"

# Internal config key for the companion entity ID (injected by post-validator).
CONF_DEVICE_NAME_SENSOR_ID = "_device_name_sensor_id"

IOHomeLock = home_io_control_ns.class_("IOHomeLock", lock.Lock, cg.Component)
IOHomeDeviceNameTextSensor = home_io_control_ns.class_(
    "IOHomeDeviceNameTextSensor", text_sensor.TextSensor, cg.Component
)


def device_name_sensor_name(config):
    """Derive the device-name sensor name from the parent entity name."""
    base_name = config.get(CONF_NAME, "")
    if base_name:
        return f"{base_name} Device Name"
    return "Device Name"


def _inject_companion_ids(config):
    """Declare companion entity IDs during schema validation for StaticVector sizing.

    ESPHome 2026.x sizes its runtime component vector (StaticVector) from the number of
    component IDs known at the end of schema validation — before to_code() runs.  If
    companion entities are only created inside to_code(), their IDs are not counted and
    the StaticVector overflows at runtime, silently dropping later components whose
    setup() then never executes.

    This post-validator injects declared IDs into the config dict so ESPHome's core
    infrastructure counts them toward ESPHOME_COMPONENT_COUNT.  The actual entity objects
    are still constructed in to_code() using these pre-declared IDs.
    """
    from esphome.helpers import sanitize

    parent_id = config[CONF_ID]
    # When no explicit id: is given, ESPHome auto-generates it after validation.
    # At this point .id may still be None, so derive from the entity name instead.
    base = parent_id.id if parent_id.id else sanitize(config[CONF_NAME]).lower()
    config[CONF_DEVICE_NAME_SENSOR_ID] = ID(
        f"{base}_device_name_sensor",
        is_declaration=True,
        type=IOHomeDeviceNameTextSensor,
    )
    return config


CONFIG_SCHEMA = cv.All(
    lock.lock_schema(IOHomeLock)
    .extend(
        {
            cv.Required(CONF_NAME): cv.string,
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
    .extend(cv.COMPONENT_SCHEMA),
    _inject_companion_ids,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await lock.register_lock(var, config)

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
        CONF_ID: config[CONF_DEVICE_NAME_SENSOR_ID],
        CONF_NAME: device_name_sensor_name(config),
        CONF_DISABLED_BY_DEFAULT: True,
        "entity_category": ENTITY_CATEGORY_DIAGNOSTIC,
    }
    device_name = await text_sensor.new_text_sensor(device_name_config)
    await cg.register_component(device_name, device_name_config)
    cg.add(device_name.set_parent(parent))
    cg.add(device_name.set_device_id(config[CONF_DEVICE_ID]))
