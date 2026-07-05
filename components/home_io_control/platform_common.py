## @file
## @brief Shared ESPHome codegen for IO-Homecontrol device-bound platforms.
## @ingroup hioc_codegen
##
## cover.py, light.py, switch.py and lock.py all bind an ESPHome entity to a hub
## device: the same config keys, the same companion device-name text sensor, and the
## same to_code() wiring (set_parent / set_device_id / device type / subtype / poll
## interval / linked remotes). This module is the single home for that shared logic so
## a change lands in one place instead of four. Platform-specific pieces — entity
## construction/registration, the cover's ``invert_position`` option, and the cover's
## favorite/vent companion buttons — deliberately stay in the platform files.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
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

# Shared YAML config keys used by every device-bound platform.
CONF_DEVICE_ID = "io_device_id"
CONF_LINKED_REMOTES = "linked_remotes"
CONF_DEVICE_TYPE = "io_device_type"
CONF_SUBTYPE = "io_subtype"
CONF_STATUS_POLL_INTERVAL = "status_poll_interval"

# Internal config key for the companion device-name sensor ID (injected by post-validator).
CONF_DEVICE_NAME_SENSOR_ID = "_device_name_sensor_id"

IOHomeDeviceNameTextSensor = home_io_control_ns.class_(
    "IOHomeDeviceNameTextSensor", text_sensor.TextSensor, cg.Component
)


def device_name_sensor_name(config):
    """Derive the device-name sensor name from the parent entity name."""
    base_name = config.get(CONF_NAME, "")
    if base_name:
        return f"{base_name} Device Name"
    return "Device Name"


def companion_id_base(config, parent_id_key):
    """Return the shared ID prefix for a platform's companion entity IDs.

    ESPHome 2026.x sizes its runtime component vector (StaticVector) from the number of
    component IDs known at the end of schema validation — before to_code() runs.  If
    companion entities are only created inside to_code(), their IDs are not counted and
    the StaticVector overflows at runtime, silently dropping later components whose
    setup() then never executes.  Companion IDs must therefore be declared during
    validation, and they all share the prefix returned here.

    ``parent_id_key`` differs per platform: light reads the entity ID from
    CONF_OUTPUT_ID, while cover, switch and lock read it from CONF_ID.
    """
    from esphome.helpers import sanitize

    parent_id = config[parent_id_key]
    # When no explicit id: is given, ESPHome auto-generates it after validation.
    # At this point .id may still be None, so derive from the entity name instead.
    return parent_id.id if parent_id.id else sanitize(config[CONF_NAME]).lower()


def inject_device_name_sensor_id(config, parent_id_key):
    """Declare the companion device-name sensor ID during schema validation.

    Shared post-validator body for every device-bound platform. See companion_id_base()
    for why the ID must be declared at validation time rather than in to_code().
    """
    base = companion_id_base(config, parent_id_key)
    config[CONF_DEVICE_NAME_SENSOR_ID] = ID(
        f"{base}_device_name_sensor",
        is_declaration=True,
        type=IOHomeDeviceNameTextSensor,
    )
    return config


def platform_schema_extension():
    """Return the shared schema keys every device-bound platform extends with."""
    return {
        cv.Required(CONF_NAME): cv.string,
        cv.GenerateID(CONF_HOME_IO_CONTROL_ID): cv.use_id(IOHomeControlComponent),
        cv.Required(CONF_DEVICE_ID): validate_device_id,
        cv.Optional(CONF_DEVICE_TYPE): validate_device_type,
        cv.Optional(CONF_SUBTYPE): cv.int_range(min=0, max=63),
        cv.Optional(CONF_LINKED_REMOTES): cv.ensure_list(validate_device_id),
        cv.Optional(CONF_STATUS_POLL_INTERVAL): validate_status_poll_interval,
    }


async def wire_device_binding(var, parent, config):
    """Emit the shared to_code() wiring that binds an entity to its hub device.

    Covers set_parent / set_device_id, the optional device type / subtype / status
    poll interval, and the linked-remotes registration loop — identical across all
    four device-bound platforms.
    """
    cg.add(var.set_parent(parent))
    cg.add(var.set_device_id(config[CONF_DEVICE_ID]))

    if CONF_DEVICE_TYPE in config:
        cg.add(var.set_device_type(device_type_expression(config[CONF_DEVICE_TYPE])))
    if CONF_SUBTYPE in config:
        cg.add(var.set_subtype(config[CONF_SUBTYPE]))
    if CONF_STATUS_POLL_INTERVAL in config:
        cg.add(
            var.set_status_poll_interval(
                config[CONF_STATUS_POLL_INTERVAL].total_milliseconds
            )
        )

    if CONF_LINKED_REMOTES in config:
        for remote_id in config[CONF_LINKED_REMOTES]:
            cg.add(parent.add_linked_remote(remote_id, config[CONF_DEVICE_ID]))


async def create_device_name_sensor(config, parent):
    """Create and register the companion device-name diagnostic text sensor."""
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
