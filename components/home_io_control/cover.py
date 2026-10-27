## @file
## @brief ESPHome cover platform schema and code generation.
## @ingroup hioc_codegen
##
## Bridges the YAML ``cover:`` platform declaration to the runtime IOHomeCover entity.
## Shared device-binding logic lives in platform_common.py; cover-specific extras — the
## ``invert_position`` option, the named-position companion buttons (favorite, ventilation), the opt-in
## ``rain_sensor_poll_interval`` key with its rain binary sensor and the opt-in ``battery`` key with
## its two battery sensors — stay here.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, button, cover, switch, text_sensor
from esphome.const import (
    CONF_DEVICE_CLASS,
    CONF_DISABLED_BY_DEFAULT,
    CONF_ID,
    CONF_NAME,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_MOISTURE,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
)
from esphome.core import ID

from . import home_io_control_ns
from .hub_validators import (
    DEFAULT_COVER_DEVICE_CLASS,
    FEATURE_EXTENDED_POSITION_FRAME,
    NAMED_POSITION_FEATURES,
    device_features,
    device_type_name,
    validate_rain_sensor_poll_interval,
)
from .platform_common import (
    companion_entity_name,
    companion_id_base,
    create_companion_sensors,
    inherit_esphome_device,
    inject_companion_sensor_ids,
    platform_schema_extension,
    require_feature,
    warn_if_experimental_kind,
    wire_device_binding,
    CONF_IO_DEVICE_ID,
    CONF_DEVICE_TYPE,
    CONF_HOME_IO_CONTROL_ID,
    CONF_SUBTYPE,
)

DEPENDENCIES = ["home_io_control"]

CONF_INVERT_POSITION = "invert_position"
CONF_SILENT = "silent"
CONF_SILENT_SWITCH_ID = "_silent_switch_id"
CONF_OPTIMISTIC_STATE = "optimistic_state"
CONF_RAIN_SENSOR_POLL_INTERVAL = "rain_sensor_poll_interval"
CONF_RAIN_SENSOR_ID = "_rain_sensor_id"
CONF_BATTERY = "battery"
CONF_BATTERY_LOW_SENSOR_ID = "_battery_low_sensor_id"
CONF_BATTERY_SENSOR_ID = "_battery_sensor_id"

# Internal config keys for the cover-only companion button IDs (injected by post-validator).
CONF_FAVORITE_BUTTON_ID = "_favorite_button_id"
CONF_VENT_BUTTON_ID = "_vent_button_id"
CONF_PARTIAL_BUTTON_ID = "_partial_button_id"
CONF_PEDESTRIAN_BUTTON_ID = "_pedestrian_button_id"

IOHomeCover = home_io_control_ns.class_("IOHomeCover", cover.Cover, cg.Component)
IOHomeCoverSilentSwitch = home_io_control_ns.class_(
    "IOHomeCoverSilentSwitch", switch.Switch, cg.Component
)
IOHomeRainBinarySensor = home_io_control_ns.class_(
    "IOHomeRainBinarySensor", binary_sensor.BinarySensor, cg.Component
)
IOHomeBatteryLowBinarySensor = home_io_control_ns.class_(
    "IOHomeBatteryLowBinarySensor", binary_sensor.BinarySensor, cg.Component
)
IOHomeBatteryTextSensor = home_io_control_ns.class_(
    "IOHomeBatteryTextSensor", text_sensor.TextSensor, cg.Component
)
# One C++ class backs both cover command companions; codegen sets which command each press
# sends via set_command() (mirrors OneWayButtonAction in hub_names.py). A device-bound `button:`
# entry is deliberately never the source — see the IOHomeOneWayCommandButton comment there.
IOHomeCoverCommandButton = home_io_control_ns.class_(
    "IOHomeCoverCommandButton", button.Button, cg.Component
)
CoverCommand = home_io_control_ns.enum("CoverCommand", is_class=True)

# The companion button of each named position, keyed by the CoverCommand name (the keys of
# hub_validators.NAMED_POSITION_FEATURES): (internal config key of the injected ID, ID suffix, entity
# name suffix, disabled by default). A button exists for a cover whose kind has the position's bit.
# The ID suffix and the name are Home Assistant entity ids, so they are pinned by the py-tests.
NAMED_POSITION_BUTTONS = {
    "FAVORITE": (CONF_FAVORITE_BUTTON_ID, "favorite_button", "Favorite Position", False),
    "VENT": (CONF_VENT_BUTTON_ID, "vent_button", "Ventilation Position", False),
    # Experimental: no public source for the values, so both buttons start disabled and the user
    # enables them in Home Assistant.
    "PEDESTRIAN": (CONF_PEDESTRIAN_BUTTON_ID, "pedestrian_button", "Pedestrian Position", True),
    "PARTIAL": (CONF_PARTIAL_BUTTON_ID, "partial_button", "Partial Position", True),
}


def _declared_features(config):
    """The capability word of the declared io_device_type / io_subtype, or 0 when no type is declared."""
    if CONF_DEVICE_TYPE not in config:
        return 0
    return device_features(config[CONF_DEVICE_TYPE], config.get(CONF_SUBTYPE, 0))


def _default_device_class(config):
    """Give a cover that declares io_device_type and sets no `device_class:` the kind's Home
    Assistant class; an explicit value, including "", is kept."""
    if CONF_DEVICE_CLASS not in config and CONF_DEVICE_TYPE in config:
        default = DEFAULT_COVER_DEVICE_CLASS.get(device_type_name(config[CONF_DEVICE_TYPE]))
        if default is not None:
            config[CONF_DEVICE_CLASS] = default
    return config


def _require_silent_kind(config):
    """Reject `silent:` on a declared kind that takes no travel-speed parameter."""
    require_feature(
        config,
        CONF_SILENT,
        CONF_SILENT in config,
        required=FEATURE_EXTENDED_POSITION_FRAME,
        reason="this device takes no travel-speed parameter; remove silent:",
    )
    return config


def silent_switch_name(config):
    """Derive the silent-operation switch name from the parent cover name."""
    return companion_entity_name(config, "Silent Operation")


def rain_sensor_name(config):
    """Derive the rain binary sensor name from the parent cover name."""
    return companion_entity_name(config, "Rain sensor")


def battery_low_sensor_name(config):
    """Derive the battery-low binary sensor name from the parent cover name."""
    return companion_entity_name(config, "Battery Low")


def battery_sensor_name(config):
    """Derive the battery-level text sensor name from the parent cover name."""
    return companion_entity_name(config, "Battery")


def _inject_companion_ids(config):
    """Declare cover companion entity IDs during schema validation for StaticVector sizing.

    The named-position buttons are cover-only and gated on the declared kind's capability bits, so
    their injection stays here. The always-present companion sensor IDs are delegated to the
    shared helper. See platform_common.companion_id_base() for the StaticVector rationale.
    """
    base = companion_id_base(config, CONF_ID)

    # Named-position buttons (favourite, ventilation, ...): one per position the declared kind has.
    features = _declared_features(config)
    for key, (config_key, suffix, _name, _disabled) in NAMED_POSITION_BUTTONS.items():
        if features & NAMED_POSITION_FEATURES[key]:
            config[config_key] = ID(
                f"{base}_{suffix}",
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

    # Battery sensors — only with `battery: true`. The level is decoded from status replies the hub
    # already receives, so the key adds entities but no radio traffic.
    if config.get(CONF_BATTERY, False):
        config[CONF_BATTERY_LOW_SENSOR_ID] = ID(
            f"{base}_battery_low_sensor",
            is_declaration=True,
            type=IOHomeBatteryLowBinarySensor,
        )
        config[CONF_BATTERY_SENSOR_ID] = ID(
            f"{base}_battery_sensor",
            is_declaration=True,
            type=IOHomeBatteryTextSensor,
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
    .extend({cv.Optional(CONF_BATTERY): cv.boolean})
    .extend({cv.Optional(CONF_OPTIMISTIC_STATE, default=True): cv.boolean})
    .extend(cv.COMPONENT_SCHEMA),
    _default_device_class,
    _require_silent_kind,
    warn_if_experimental_kind,
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

    if CONF_BATTERY_LOW_SENSOR_ID in config:
        cg.add(var.set_battery_entities(True))
        await _create_battery_sensors(config, parent)

    for key, (config_key, _suffix, name, disabled) in NAMED_POSITION_BUTTONS.items():
        if config_key not in config:
            continue
        button_config = inherit_esphome_device(
            {
                CONF_ID: config[config_key],
                CONF_NAME: companion_entity_name(config, name),
                CONF_DISABLED_BY_DEFAULT: disabled,
            },
            config,
        )
        position_button = await button.new_button(button_config)
        await cg.register_component(position_button, button_config)
        cg.add(position_button.set_parent(parent))
        cg.add(position_button.set_device_id(config[CONF_IO_DEVICE_ID]))
        cg.add(position_button.set_command(getattr(CoverCommand, key)))

    await create_companion_sensors(config, parent)


async def _create_battery_sensors(config, parent):
    """Create the two `battery: true` companions.

    Battery Low is the enabled one: a `device_class: battery` binary sensor is Home Assistant's own
    low-battery signal. Battery (full/normal/low) is diagnostic and disabled by default; ESPHome
    text sensors carry no battery device class, so nothing generic reads it.
    """
    low_config = binary_sensor.binary_sensor_schema(
        IOHomeBatteryLowBinarySensor,
        device_class=DEVICE_CLASS_BATTERY,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ).extend(cv.COMPONENT_SCHEMA)(
        inherit_esphome_device(
            {
                CONF_ID: config[CONF_BATTERY_LOW_SENSOR_ID],
                CONF_NAME: battery_low_sensor_name(config),
            },
            config,
        )
    )
    low_sensor = await binary_sensor.new_binary_sensor(low_config)
    await cg.register_component(low_sensor, low_config)
    cg.add(low_sensor.set_parent(parent))
    cg.add(low_sensor.set_device_id(config[CONF_IO_DEVICE_ID]))

    level_config = text_sensor.text_sensor_schema(
        IOHomeBatteryTextSensor,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        icon="mdi:battery",
    ).extend(cv.COMPONENT_SCHEMA)(
        inherit_esphome_device(
            {
                CONF_ID: config[CONF_BATTERY_SENSOR_ID],
                CONF_NAME: battery_sensor_name(config),
                CONF_DISABLED_BY_DEFAULT: True,
            },
            config,
        )
    )
    level_sensor = await text_sensor.new_text_sensor(level_config)
    await cg.register_component(level_sensor, level_config)
    cg.add(level_sensor.set_parent(parent))
    cg.add(level_sensor.set_device_id(config[CONF_IO_DEVICE_ID]))
