## @file
## @brief ESPHome cover platform schema and code generation.
## @ingroup hioc_codegen
##
## Bridges the YAML ``cover:`` platform declaration to the runtime IOHomeCover entity.
## Shared device-binding logic lives in platform_common.py; cover-specific extras — the
## ``invert_position`` option, the favorite/ventilation companion buttons and the opt-in
## ``rain_sensor_poll_interval`` key with its rain binary sensor — stay here.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, button, cover, switch
from esphome.const import (
    CONF_DISABLED_BY_DEFAULT,
    CONF_ID,
    CONF_NAME,
    DEVICE_CLASS_MOISTURE,
    ENTITY_CATEGORY_CONFIG,
)
from esphome.core import ID

from . import home_io_control_ns
from .hub_validators import validate_rain_sensor_poll_interval
from .platform_common import (
    companion_entity_name,
    companion_id_base,
    create_companion_sensors,
    inherit_esphome_device,
    inject_companion_sensor_ids,
    platform_schema_extension,
    wire_device_binding,
    CONF_IO_DEVICE_ID,
    CONF_DEVICE_TYPE,
    CONF_HOME_IO_CONTROL_ID,
)

DEPENDENCIES = ["home_io_control"]

CONF_INVERT_POSITION = "invert_position"
CONF_SILENT = "silent"
CONF_SILENT_SWITCH_ID = "_silent_switch_id"
CONF_OPTIMISTIC_STATE = "optimistic_state"
CONF_RAIN_SENSOR_POLL_INTERVAL = "rain_sensor_poll_interval"
CONF_RAIN_SENSOR_ID = "_rain_sensor_id"

# Internal config keys for the cover-only companion button IDs (injected by post-validator).
CONF_FAVORITE_BUTTON_ID = "_favorite_button_id"
CONF_VENT_BUTTON_ID = "_vent_button_id"

IOHomeCover = home_io_control_ns.class_("IOHomeCover", cover.Cover, cg.Component)
IOHomeCoverSilentSwitch = home_io_control_ns.class_(
    "IOHomeCoverSilentSwitch", switch.Switch, cg.Component
)
IOHomeRainBinarySensor = home_io_control_ns.class_(
    "IOHomeRainBinarySensor", binary_sensor.BinarySensor, cg.Component
)
# One C++ class backs both cover command companions; codegen sets which command each press
# sends via set_command() (mirrors OneWayButtonAction in hub_names.py). A device-bound `button:`
# entry is deliberately never the source — see the IOHomeOneWayCommandButton comment there.
IOHomeCoverCommandButton = home_io_control_ns.class_(
    "IOHomeCoverCommandButton", button.Button, cg.Component
)
CoverCommand = home_io_control_ns.enum("CoverCommand", is_class=True)

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
    0x1D,  # bioclimatic_pergola
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
    return companion_entity_name(config, "Favorite Position")


def vent_button_name(config):
    """Derive the ventilation-position button name from the parent cover name."""
    return companion_entity_name(config, "Ventilation Position")


def silent_switch_name(config):
    """Derive the silent-operation switch name from the parent cover name."""
    return companion_entity_name(config, "Silent Operation")


def rain_sensor_name(config):
    """Derive the rain binary sensor name from the parent cover name."""
    return companion_entity_name(config, "Rain sensor")


def _inject_companion_ids(config):
    """Declare cover companion entity IDs during schema validation for StaticVector sizing.

    The favorite and ventilation buttons are cover-only and gated on device capability, so
    their injection stays here. The always-present companion sensor IDs are delegated to the
    shared helper. See platform_common.companion_id_base() for the StaticVector rationale.
    """
    base = companion_id_base(config, CONF_ID)

    # Favorite-position button — only for position-capable device types.
    if CONF_DEVICE_TYPE in config and device_supports_position_control(
        config[CONF_DEVICE_TYPE]
    ):
        config[CONF_FAVORITE_BUTTON_ID] = ID(
            f"{base}_favorite_button",
            is_declaration=True,
            type=IOHomeCoverCommandButton,
        )

    # Ventilation-position button — only for window-type device types.
    if CONF_DEVICE_TYPE in config and device_supports_vent(config[CONF_DEVICE_TYPE]):
        config[CONF_VENT_BUTTON_ID] = ID(
            f"{base}_vent_button",
            is_declaration=True,
            type=IOHomeCoverCommandButton,
        )

    # Silent-operation toggle — only when the cover declares `silent:` at all. Declaring the key
    # is what opts a cover into runtime control of its travel profile; a config that never mentions
    # it gains no entity, and the YAML value is simply the boot state.
    if CONF_SILENT in config:
        config[CONF_SILENT_SWITCH_ID] = ID(
            f"{base}_silent_switch",
            is_declaration=True,
            type=IOHomeCoverSilentSwitch,
        )

    # Rain binary sensor — only when the cover declares `rain_sensor_poll_interval:`. The key is
    # the opt-in to the periodic limitation read, so a config without it gains no entity.
    if CONF_RAIN_SENSOR_POLL_INTERVAL in config:
        config[CONF_RAIN_SENSOR_ID] = ID(
            f"{base}_rain_sensor",
            is_declaration=True,
            type=IOHomeRainBinarySensor,
        )

    # Companion diagnostic sensors — always generated (shared with other platforms).
    return inject_companion_sensor_ids(config, CONF_ID)


CONFIG_SCHEMA = cv.All(
    cover.cover_schema(IOHomeCover)
    .extend(platform_schema_extension())
    .extend({cv.Optional(CONF_INVERT_POSITION): cv.boolean})
    .extend({cv.Optional(CONF_SILENT): cv.boolean})
    .extend(
        {
            cv.Optional(
                CONF_RAIN_SENSOR_POLL_INTERVAL
            ): validate_rain_sensor_poll_interval
        }
    )
    .extend({cv.Optional(CONF_OPTIMISTIC_STATE, default=True): cv.boolean})
    .extend(cv.COMPONENT_SCHEMA),
    _inject_companion_ids,
)


async def to_code(config):
    var = await cover.new_cover(config)
    await cg.register_component(var, config)

    parent = await cg.get_variable(config[CONF_HOME_IO_CONTROL_ID])
    await wire_device_binding(var, parent, config)

    if CONF_INVERT_POSITION in config:
        cg.add(var.set_invert_position(config[CONF_INVERT_POSITION]))

    cg.add(var.set_optimistic_state(config[CONF_OPTIMISTIC_STATE]))
    if CONF_SILENT in config:
        cg.add(var.set_silent(config[CONF_SILENT]))

    if CONF_SILENT_SWITCH_ID in config:
        # Built through switch_schema()+COMPONENT_SCHEMA so it carries the entity/component
        # defaults register_switch() requires, matching _create_accept_foreign_pairing_switch().
        # Restore mode DISABLED on purpose: the YAML `silent:` value is the boot state, and
        # setup() publishes it. Any restoring mode would either fight that or drive write_state()
        # before the device is even registered.
        silent_config = switch.switch_schema(
            IOHomeCoverSilentSwitch,
            default_restore_mode="DISABLED",
            entity_category=ENTITY_CATEGORY_CONFIG,
        ).extend(cv.COMPONENT_SCHEMA)(
            inherit_esphome_device(
                {
                    CONF_ID: config[CONF_SILENT_SWITCH_ID],
                    CONF_NAME: silent_switch_name(config),
                },
                config,
            )
        )
        silent_switch = await switch.new_switch(silent_config)
        await cg.register_component(silent_switch, silent_config)
        cg.add(silent_switch.set_parent(parent))
        cg.add(silent_switch.set_device_id(config[CONF_IO_DEVICE_ID]))

    if CONF_RAIN_SENSOR_ID in config:
        cg.add(
            var.set_rain_sensor_poll_interval(
                config[CONF_RAIN_SENSOR_POLL_INTERVAL].total_milliseconds
            )
        )
        rain_config = binary_sensor.binary_sensor_schema(
            IOHomeRainBinarySensor, device_class=DEVICE_CLASS_MOISTURE
        ).extend(cv.COMPONENT_SCHEMA)(
            inherit_esphome_device(
                {
                    CONF_ID: config[CONF_RAIN_SENSOR_ID],
                    CONF_NAME: rain_sensor_name(config),
                },
                config,
            )
        )
        rain_sensor = await binary_sensor.new_binary_sensor(rain_config)
        await cg.register_component(rain_sensor, rain_config)
        cg.add(rain_sensor.set_parent(parent))
        cg.add(rain_sensor.set_device_id(config[CONF_IO_DEVICE_ID]))

    if CONF_FAVORITE_BUTTON_ID in config:
        favorite_config = inherit_esphome_device(
            {
                CONF_ID: config[CONF_FAVORITE_BUTTON_ID],
                CONF_NAME: favorite_button_name(config),
                CONF_DISABLED_BY_DEFAULT: False,
            },
            config,
        )
        favorite = await button.new_button(favorite_config)
        await cg.register_component(favorite, favorite_config)
        cg.add(favorite.set_parent(parent))
        cg.add(favorite.set_device_id(config[CONF_IO_DEVICE_ID]))
        cg.add(favorite.set_command(CoverCommand.FAVORITE))

    if CONF_VENT_BUTTON_ID in config:
        vent_config = inherit_esphome_device(
            {
                CONF_ID: config[CONF_VENT_BUTTON_ID],
                CONF_NAME: vent_button_name(config),
                CONF_DISABLED_BY_DEFAULT: False,
            },
            config,
        )
        vent = await button.new_button(vent_config)
        await cg.register_component(vent, vent_config)
        cg.add(vent.set_parent(parent))
        cg.add(vent.set_device_id(config[CONF_IO_DEVICE_ID]))
        cg.add(vent.set_command(CoverCommand.VENT))

    await create_companion_sensors(config, parent)
