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
from esphome.core import ID

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

# Internal config keys for companion entity IDs (injected by post-validator).
CONF_FAVORITE_BUTTON_ID = "_favorite_button_id"
CONF_VENT_BUTTON_ID = "_vent_button_id"
CONF_DEVICE_NAME_SENSOR_ID = "_device_name_sensor_id"

IOHomeCover = home_io_control_ns.class_("IOHomeCover", cover.Cover, cg.Component)
IOHomeCoverFavoriteButton = home_io_control_ns.class_(
    "IOHomeCoverFavoriteButton", button.Button, cg.Component
)
IOHomeCoverVentButton = home_io_control_ns.class_(
    "IOHomeCoverVentButton", button.Button, cg.Component
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


# Device types that support the ventilation position command.
# These are window-type actuators that can move to a predefined vent opening.
VENT_DEVICE_TYPES = {
    0x04,  # window_opener
    0x14,  # ventilation_point
}


def device_supports_vent(value):
    """Check if the given device type value supports the ventilation command."""
    return value in VENT_DEVICE_TYPES


def favorite_button_name(config):
    """Derive the favorite-position button name from the parent cover name."""
    base_name = config.get(CONF_NAME, "")
    if base_name:
        return f"{base_name} Favorite Position"
    return "Favorite Position"


def vent_button_name(config):
    """Derive the ventilation-position button name from the parent cover name."""
    base_name = config.get(CONF_NAME, "")
    if base_name:
        return f"{base_name} Ventilation Position"
    return "Ventilation Position"


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

    # Favorite-position button — only for position-capable device types.
    if CONF_DEVICE_TYPE in config and device_supports_position_control(
        config[CONF_DEVICE_TYPE]
    ):
        config[CONF_FAVORITE_BUTTON_ID] = ID(
            f"{base}_favorite_button",
            is_declaration=True,
            type=IOHomeCoverFavoriteButton,
        )

    # Ventilation-position button — only for window-type device types.
    if CONF_DEVICE_TYPE in config and device_supports_vent(
        config[CONF_DEVICE_TYPE]
    ):
        config[CONF_VENT_BUTTON_ID] = ID(
            f"{base}_vent_button",
            is_declaration=True,
            type=IOHomeCoverVentButton,
        )

    # Device-name diagnostic text sensor — always generated.
    config[CONF_DEVICE_NAME_SENSOR_ID] = ID(
        f"{base}_device_name_sensor",
        is_declaration=True,
        type=IOHomeDeviceNameTextSensor,
    )

    return config


CONFIG_SCHEMA = cv.All(
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
    .extend(cv.COMPONENT_SCHEMA),
    _inject_companion_ids,
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

    if CONF_FAVORITE_BUTTON_ID in config:
        favorite_config = {
            CONF_ID: config[CONF_FAVORITE_BUTTON_ID],
            CONF_NAME: favorite_button_name(config),
            CONF_DISABLED_BY_DEFAULT: False,
        }
        favorite = await button.new_button(favorite_config)
        await cg.register_component(favorite, favorite_config)
        cg.add(favorite.set_parent(parent))
        cg.add(favorite.set_device_id(config[CONF_DEVICE_ID]))

    if CONF_VENT_BUTTON_ID in config:
        vent_config = {
            CONF_ID: config[CONF_VENT_BUTTON_ID],
            CONF_NAME: vent_button_name(config),
            CONF_DISABLED_BY_DEFAULT: False,
        }
        vent = await button.new_button(vent_config)
        await cg.register_component(vent, vent_config)
        cg.add(vent.set_parent(parent))
        cg.add(vent.set_device_id(config[CONF_DEVICE_ID]))

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
