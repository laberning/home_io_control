## @file
## @brief ESPHome cover platform schema and code generation.
## @ingroup hioc_codegen
##
## Bridges the YAML ``cover:`` platform declaration to the runtime IOHomeCover entity.
## Shared device-binding logic lives in platform_common.py; cover-specific extras — the
## ``invert_position`` option, the named-position companion buttons (favorite, ventilation), the
## ``silent`` and ``travel_speed`` keys with their switch and select, the opt-in
## ``rain_sensor_poll_interval`` key, the opt-in ``sun_level_poll_interval`` and ``battery_level_poll_interval``
## keys, the opt-in ``status_refresh_interval`` key and the opt-in ``battery`` key stay here. The entities those
## keys declare (rain, sun level, battery level, battery state, battery low) are readings of DEVICE_READINGS
## (hub_validators.py), built and scheduled by platform_common.py from the mirror's ``opt_in_key``.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button, cover, select, switch
from esphome.const import (
    CONF_DEVICE_CLASS,
    CONF_DISABLED_BY_DEFAULT,
    CONF_ID,
    ENTITY_CATEGORY_CONFIG,
)

from . import home_io_control_ns
from .hub_names import CONF_INVERT_POSITION
from .hub_validators import (
    DEFAULT_COVER_DEVICE_CLASS,
    NAMED_POSITION_FEATURES,
    device_type_name,
    reading_poll_interval_schema,
    validate_status_refresh_interval,
)
from .platform_common import (
    COVER_PLATFORM,
    companion_entity_name,
    create_companion,
    create_companion_sensors,
    deprecated_key,
    has_feature,
    inject_companion_ids,
    inject_companion_sensor_ids,
    inject_reading_companion_ids,
    platform_schema_extension,
    reading_companions_to_code,
    reading_schedules_to_code,
    require_feature,
    validate_device_model,
    warn_if_experimental_kind,
    wire_device_binding,
    CONF_DEVICE_TYPE,
    CONF_HOME_IO_CONTROL_ID,
)

DEPENDENCIES = ["home_io_control"]

CONF_SILENT = "silent"
CONF_SILENT_SWITCH_ID = "_silent_switch_id"
CONF_TRAVEL_SPEED = "travel_speed"
CONF_TRAVEL_SPEED_SELECT_ID = "_travel_speed_select_id"
CONF_OPTIMISTIC_STATE = "optimistic_state"
CONF_STATUS_REFRESH_INTERVAL = "status_refresh_interval"
CONF_BATTERY = "battery"

# Internal config keys for the cover-only companion button IDs (injected by post-validator).
CONF_FAVORITE_BUTTON_ID = "_favorite_button_id"
CONF_VENT_BUTTON_ID = "_vent_button_id"
CONF_PARTIAL_BUTTON_ID = "_partial_button_id"
CONF_PEDESTRIAN_BUTTON_ID = "_pedestrian_button_id"

IOHomeCover = home_io_control_ns.class_("IOHomeCover", cover.Cover, cg.Component)
IOHomeCoverSilentSwitch = home_io_control_ns.class_(
    "IOHomeCoverSilentSwitch", switch.Switch, cg.Component
)
IOHomeCoverTravelSpeedSelect = home_io_control_ns.class_(
    "IOHomeCoverTravelSpeedSelect", select.Select, cg.Component
)
TravelSpeed = home_io_control_ns.enum("TravelSpeed", is_class=True)

# The `travel_speed:` values, which are also the select's options in this order. The names are the
# ones travel_speed_name() publishes in C++, so the select's state reads back as a valid YAML value.
TRAVEL_SPEEDS = {
    "default": TravelSpeed.MOTOR_DEFAULT,
    "silent": TravelSpeed.SILENT,
    "fast": TravelSpeed.FAST,
}
TRAVEL_SPEED_EXCLUSIVE_MESSAGE = (
    "silent: and travel_speed: set the same thing; keep one "
    "(silent: true is travel_speed: silent, silent: false is travel_speed: fast)"
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
    # Disabled by default: the pedestrian value is experimental (no public source), and a garage
    # opener runs only the partial position stored in it (issue #163). The user enables them in
    # Home Assistant.
    "PEDESTRIAN": (CONF_PEDESTRIAN_BUTTON_ID, "pedestrian_button", "Pedestrian Position", True),
    "PARTIAL": (CONF_PARTIAL_BUTTON_ID, "partial_button", "Partial Position", True),
}


def _default_device_class(config):
    """Give a cover that declares io_device_type and sets no `device_class:` the kind's Home
    Assistant class; an explicit value, including "", is kept."""
    if CONF_DEVICE_CLASS not in config and CONF_DEVICE_TYPE in config:
        default = DEFAULT_COVER_DEVICE_CLASS.get(device_type_name(config[CONF_DEVICE_TYPE]))
        if default is not None:
            config[CONF_DEVICE_CLASS] = default
    return config


def _require_travel_speed_kind(config):
    """Reject `silent:` or `travel_speed:` on a declared kind that takes no travel-speed parameter."""
    for key in (CONF_SILENT, CONF_TRAVEL_SPEED):
        require_feature(
            config,
            key,
            key in config,
            required="EXTENDED_POSITION_FRAME",
            reason=f"this device takes no travel-speed parameter; remove {key}:",
        )
    return config


def silent_switch_name(config):
    """Derive the silent-operation switch name from the parent cover name."""
    return companion_entity_name(config, "Silent Operation")


def travel_speed_select_name(config):
    """Derive the travel speed select name from the parent cover name."""
    return companion_entity_name(config, "Travel Speed")


# The cover's companion entities: (id key, ID suffix, class, condition), declared at validation time
# and created in to_code() from the same id key. The shared diagnostic sensors are not listed; they
# come from inject_companion_sensor_ids(), the entities of the opt-in readings (rain, sun level, battery) from
# inject_reading_companion_ids(). The ID suffixes are Home Assistant entity ids, so they are pinned by the py-tests.
COVER_COMPANIONS = (
    # Named-position buttons (favourite, ventilation, ...): one per position the declared kind has.
    *(
        (config_key, suffix, IOHomeCoverCommandButton, has_feature(NAMED_POSITION_FEATURES[key]))
        for key, (config_key, suffix, _name, _disabled) in NAMED_POSITION_BUTTONS.items()
    ),
    # Silent-operation toggle: only when the cover declares `silent:` at all. Declaring the key is
    # what opts a cover into runtime control of its travel profile; a config that never mentions it
    # gains no entity, and the YAML value is simply the boot state.
    (CONF_SILENT_SWITCH_ID, "silent_switch", IOHomeCoverSilentSwitch, CONF_SILENT),
    # Travel speed select: likewise only with `travel_speed:`, the three-way form of the same
    # setting. The two keys are exclusive, so a cover has one entity or the other.
    (CONF_TRAVEL_SPEED_SELECT_ID, "travel_speed_select", IOHomeCoverTravelSpeedSelect, CONF_TRAVEL_SPEED),
)


def _inject_companion_ids(config):
    """Declare the cover companion entity IDs during schema validation for StaticVector sizing.

    The cover's own companions come from COVER_COMPANIONS, the entities of its opt-in readings from the
    reading mirror and the always-present diagnostic sensors from the shared table. See
    platform_common.companion_id_base() for the StaticVector rationale.
    """
    inject_companion_ids(config, COVER_COMPANIONS, CONF_ID)
    inject_reading_companion_ids(config, CONF_ID, COVER_PLATFORM)
    return inject_companion_sensor_ids(config, CONF_ID)


def _silent_replacement(config):
    """What a cover with `silent:` writes instead, and what changes in Home Assistant."""
    speed = "silent" if config[CONF_SILENT] else "fast"
    return (
        f"Replace 'silent: {str(config[CONF_SILENT]).lower()}' with 'travel_speed: {speed}'. The "
        f"'{silent_switch_name(config)}' switch is then replaced by a '{travel_speed_select_name(config)}' "
        "select (default, silent, fast): update any automation or dashboard that uses the switch."
    )


CONFIG_SCHEMA = cv.All(
    cover.cover_schema(IOHomeCover)
    .extend(platform_schema_extension())
    .extend({cv.Optional(CONF_INVERT_POSITION): cv.boolean})
    .extend(
        {
            # Both set the travel speed: `silent:` two-way, `travel_speed:` three-way.
            cv.Exclusive(
                CONF_SILENT, "travel_speed", msg=TRAVEL_SPEED_EXCLUSIVE_MESSAGE
            ): cv.boolean,
            cv.Exclusive(
                CONF_TRAVEL_SPEED, "travel_speed", msg=TRAVEL_SPEED_EXCLUSIVE_MESSAGE
            ): cv.enum(TRAVEL_SPEEDS, lower=True),
        }
    )
    # The keys of a cover's scheduled readings (rain, sun level, battery level) and their floor come from the mirror.
    .extend(reading_poll_interval_schema(COVER_PLATFORM))
    .extend({cv.Optional(CONF_STATUS_REFRESH_INTERVAL): validate_status_refresh_interval})
    .extend({cv.Optional(CONF_BATTERY): cv.boolean})
    .extend({cv.Optional(CONF_OPTIMISTIC_STATE, default=True): cv.boolean})
    .extend(cv.COMPONENT_SCHEMA),
    validate_device_model,
    _default_device_class,
    _require_travel_speed_kind,
    deprecated_key(CONF_SILENT, _silent_replacement),
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
        speed = TravelSpeed.SILENT if config[CONF_SILENT] else TravelSpeed.FAST
        cg.add(var.set_travel_speed(speed))
    if CONF_TRAVEL_SPEED in config:
        cg.add(var.set_travel_speed(config[CONF_TRAVEL_SPEED]))

    if CONF_SILENT_SWITCH_ID in config:
        # Restore mode DISABLED on purpose: the YAML `silent:` value is the boot state, and
        # setup() publishes it. Any restoring mode would either fight that or drive write_state()
        # before the device is even registered.
        await create_companion(
            config,
            parent,
            schema=switch.switch_schema(
                IOHomeCoverSilentSwitch,
                default_restore_mode="DISABLED",
                entity_category=ENTITY_CATEGORY_CONFIG,
            ),
            new_entity=switch.new_switch,
            id_key=CONF_SILENT_SWITCH_ID,
            name=silent_switch_name(config),
        )

    if CONF_TRAVEL_SPEED_SELECT_ID in config:
        # Nothing restores it: the YAML `travel_speed:` value is the boot state, as for the switch
        # above, and setup() publishes it.
        await create_companion(
            config,
            parent,
            schema=select.select_schema(
                IOHomeCoverTravelSpeedSelect, entity_category=ENTITY_CATEGORY_CONFIG
            ),
            new_entity=select.new_select,
            id_key=CONF_TRAVEL_SPEED_SELECT_ID,
            name=travel_speed_select_name(config),
            options=list(TRAVEL_SPEEDS),
        )

    # The opt-in readings: their entities, then their schedules (rain, sun level and battery level are read on one).
    await reading_companions_to_code(config, parent, COVER_PLATFORM)
    reading_schedules_to_code(var, config, COVER_PLATFORM)

    if CONF_STATUS_REFRESH_INTERVAL in config:
        cg.add(
            var.set_status_refresh_interval(
                config[CONF_STATUS_REFRESH_INTERVAL].total_milliseconds
            )
        )

    if config.get(CONF_BATTERY, False):
        cg.add(var.set_battery_entities(True))

    for key, (config_key, _suffix, name, disabled) in NAMED_POSITION_BUTTONS.items():
        if config_key not in config:
            continue
        position_button = await create_companion(
            config,
            parent,
            schema=button.button_schema(IOHomeCoverCommandButton),
            new_entity=button.new_button,
            id_key=config_key,
            name=companion_entity_name(config, name),
            overrides={CONF_DISABLED_BY_DEFAULT: disabled},
        )
        cg.add(position_button.set_command(getattr(CoverCommand, key)))

    await create_companion_sensors(config, parent)
