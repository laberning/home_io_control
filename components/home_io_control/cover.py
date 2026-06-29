## @file
## @brief ESPHome cover platform schema and code generation.
## @ingroup hioc_codegen
##
## Bridges the YAML ``cover:`` platform declaration to the runtime IOHomeCover entity.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button, cover, text_sensor
from esphome.const import (
    CONF_DISABLED_BY_DEFAULT,
    CONF_ID,
    CONF_NAME,
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
CONF_INVERT_POSITION = "invert_position"
CONF_LINKED_REMOTES = "linked_remotes"
CONF_DEVICE_TYPE = "io_device_type"
CONF_SUBTYPE = "io_subtype"
CONF_STATUS_POLL_INTERVAL = "status_poll_interval"

IOHomeCover = home_io_control_ns.class_("IOHomeCover", cover.Cover, cg.Component)
IOHomeCoverFavoriteButton = home_io_control_ns.class_(
    "IOHomeCoverFavoriteButton", button.Button, cg.Component
)
IOHomeDeviceNameTextSensor = home_io_control_ns.class_(
    "IOHomeDeviceNameTextSensor", text_sensor.TextSensor, cg.Component
)

# Device types that support 0-100% position control (maps to DeviceCapabilityClass::COVER in C++).
# Used to decide whether a favorite-position button companion should be generated.
POSITION_CONTROL_DEVICE_TYPES = {
    0x01,  # venetian_blind
    0x02,  # roller_shutter
    0x03,  # awning
    0x04,  # window_opener
    0x05,  # garage_opener
    0x07,  # gate_opener
    0x08,  # rolling_door_opener
    0x0A,  # blind
    0x0B,  # screen
    0x0D,  # dual_shutter
    0x10,  # horizontal_awning
    0x11,  # external_venetian_blind
    0x12,  # louvre_blind
    0x13,  # curtain_track
    0x18,  # swinging_shutter
}


def device_supports_position_control(value):
    """Check if the given device type value supports 0-100% position control."""
    return value in POSITION_CONTROL_DEVICE_TYPES


def favorite_button_id(parent_id):
    """Generate a unique ID for the favorite-position button child entity."""
    return ID(
        f"{parent_id.id}_favorite_button",
        is_declaration=True,
        type=IOHomeCoverFavoriteButton,
    )


def favorite_button_name(config):
    """Derive the favorite-position button name from the parent cover name."""
    base_name = config.get(CONF_NAME, "")
    if base_name:
        return f"{base_name} Favorite Position"
    return "Favorite Position"


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
    cover.cover_schema(IOHomeCover)
    .extend(
        {
            cv.Required(CONF_NAME): cv.string,
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

    if CONF_DEVICE_TYPE in config and device_supports_position_control(
        config[CONF_DEVICE_TYPE]
    ):
        favorite_config = {
            CONF_ID: favorite_button_id(config[CONF_ID]),
            CONF_NAME: favorite_button_name(config),
            CONF_DISABLED_BY_DEFAULT: False,
        }
        favorite = await button.new_button(favorite_config)
        CORE.component_ids.add(str(favorite.base))
        await cg.register_component(favorite, favorite_config)
        cg.add(favorite.set_parent(parent))
        cg.add(favorite.set_device_id(config[CONF_DEVICE_ID]))

    device_name_config = {
        CONF_ID: device_name_sensor_id(config[CONF_ID]),
        CONF_NAME: device_name_sensor_name(config),
        CONF_DISABLED_BY_DEFAULT: True,
        "entity_category": ENTITY_CATEGORY_DIAGNOSTIC,
    }
    device_name = await text_sensor.new_text_sensor(device_name_config)
    CORE.component_ids.add(str(device_name.base))
    await cg.register_component(device_name, device_name_config)
    cg.add(device_name.set_parent(parent))
    cg.add(device_name.set_device_id(config[CONF_DEVICE_ID]))
