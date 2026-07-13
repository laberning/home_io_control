## @file
## @brief Shared ESPHome codegen for IO-Homecontrol device-bound platforms.
## @ingroup hioc_codegen
##
## cover.py, light.py, switch.py and lock.py all bind an ESPHome entity to a hub
## device: the same config keys, the same auto-generated companion diagnostic sensors
## (device name, last result, RSSI, last seen, exchange failures), and the same
## to_code() wiring (set_parent / set_device_id / device type / subtype / poll
## interval / linked remotes). This module is the single home for that shared logic so
## a change lands in one place instead of four — platform files call one
## inject_companion_sensor_ids() post-validator and one create_companion_sensors()
## to_code() helper. Platform-specific pieces — entity construction/registration, the
## cover's ``invert_position`` option, and the cover's favorite/vent companion
## buttons — deliberately stay in the platform files.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor, text_sensor
from esphome.const import (
    CONF_ACCURACY_DECIMALS,
    CONF_DEVICE_CLASS,
    CONF_DISABLED_BY_DEFAULT,
    CONF_FORCE_UPDATE,
    CONF_ID,
    CONF_NAME,
    CONF_STATE_CLASS,
    CONF_UNIT_OF_MEASUREMENT,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
)
from esphome.components.sensor import DEVICE_CLASS_SIGNAL_STRENGTH
from esphome.core import ID

from . import (
    home_io_control_ns,
    IOHomeControlComponent,
    CONF_HOME_IO_CONTROL_ID,
    device_type_expression,
    validate_device_id,
    validate_device_type,
    validate_linked_remote_entry,
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
# Internal config key for the companion last-result sensor ID (injected by post-validator).
CONF_LAST_RESULT_SENSOR_ID = "_last_result_sensor_id"
# Internal config keys for the companion link-health sensor IDs (injected by post-validator).
CONF_RSSI_SENSOR_ID = "_rssi_sensor_id"
CONF_LAST_SEEN_SENSOR_ID = "_last_seen_sensor_id"
CONF_EXCHANGE_FAILURES_SENSOR_ID = "_exchange_failures_sensor_id"

IOHomeDeviceNameTextSensor = home_io_control_ns.class_(
    "IOHomeDeviceNameTextSensor", text_sensor.TextSensor, cg.Component
)
IOHomeLastResultTextSensor = home_io_control_ns.class_(
    "IOHomeLastResultTextSensor", text_sensor.TextSensor, cg.Component
)
IOHomeRssiSensor = home_io_control_ns.class_("IOHomeRssiSensor", sensor.Sensor, cg.Component)
IOHomeLastSeenSensor = home_io_control_ns.class_(
    "IOHomeLastSeenSensor", sensor.Sensor, cg.Component
)
IOHomeExchangeFailuresSensor = home_io_control_ns.class_(
    "IOHomeExchangeFailuresSensor", sensor.Sensor, cg.Component
)


# (config key, ID suffix, codegen class) for every auto-generated companion sensor.
_COMPANION_SENSOR_IDS = (
    (CONF_DEVICE_NAME_SENSOR_ID, "device_name_sensor", IOHomeDeviceNameTextSensor),
    (CONF_LAST_RESULT_SENSOR_ID, "last_result_sensor", IOHomeLastResultTextSensor),
    (CONF_RSSI_SENSOR_ID, "rssi_sensor", IOHomeRssiSensor),
    (CONF_LAST_SEEN_SENSOR_ID, "last_seen_sensor", IOHomeLastSeenSensor),
    (CONF_EXCHANGE_FAILURES_SENSOR_ID, "exchange_failures_sensor", IOHomeExchangeFailuresSensor),
)


def _companion_sensor_name(config, suffix):
    """Derive a companion sensor's entity name from the parent entity name."""
    base_name = config.get(CONF_NAME, "")
    if base_name:
        return f"{base_name} {suffix}"
    return suffix


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


def inject_companion_sensor_ids(config, parent_id_key):
    """Declare every auto-generated companion sensor ID during schema validation.

    Shared post-validator body for every device-bound platform, covering all entries in
    _COMPANION_SENSOR_IDS. See companion_id_base() for why the IDs must be declared at
    validation time rather than in to_code().
    """
    base = companion_id_base(config, parent_id_key)
    for conf_key, id_suffix, sensor_class in _COMPANION_SENSOR_IDS:
        config[conf_key] = ID(
            f"{base}_{id_suffix}", is_declaration=True, type=sensor_class
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
        cv.Optional(CONF_LINKED_REMOTES): cv.ensure_list(validate_linked_remote_entry),
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
            if remote_id.startswith("class:"):
                # validate_linked_remote_entry() already normalized this to 'class:0x<HH>'.
                type_value = int(remote_id.split(":", 1)[1], 16)
                cg.add(
                    parent.add_linked_remote_class(
                        device_type_expression(type_value),
                        config[CONF_DEVICE_ID],
                    )
                )
            else:
                cg.add(parent.add_linked_remote(remote_id, config[CONF_DEVICE_ID]))


async def _create_companion_text_sensor(config, parent, sensor_id, name, disabled_by_default):
    """Shared body for the auto-generated companion `text_sensor:` entities."""
    companion_config = {
        CONF_ID: sensor_id,
        CONF_NAME: name,
        CONF_DISABLED_BY_DEFAULT: disabled_by_default,
        "entity_category": ENTITY_CATEGORY_DIAGNOSTIC,
    }
    companion = await text_sensor.new_text_sensor(companion_config)
    await cg.register_component(companion, companion_config)
    cg.add(companion.set_parent(parent))
    cg.add(companion.set_device_id(config[CONF_DEVICE_ID]))


async def _create_link_health_sensor(config, parent, sensor_id, name, **sensor_kwargs):
    """Shared body for the three auto-generated link-health `sensor:` companions.

    All three (RSSI, Last Seen, Exchange Failures) are numeric, diagnostic, and disabled by
    default (noise control); only the name and sensor-specific schema keys
    (unit/device_class/state_class/accuracy_decimals) differ between them, so those are the
    only things each call in create_companion_sensors() supplies.
    """
    if CONF_STATE_CLASS in sensor_kwargs:
        # set_state_class() takes a C++ enum, not a raw string; validate_state_class() is what
        # normal YAML schema validation would apply to turn the string constant into the
        # EnumValue codegen expects. We build this dict by hand rather than running it through
        # sensor_schema(), so it must be applied here.
        sensor_kwargs[CONF_STATE_CLASS] = sensor.validate_state_class(sensor_kwargs[CONF_STATE_CLASS])

    link_health_config = {
        CONF_ID: sensor_id,
        CONF_NAME: name,
        CONF_DISABLED_BY_DEFAULT: True,
        "entity_category": ENTITY_CATEGORY_DIAGNOSTIC,
        CONF_FORCE_UPDATE: False,
        **sensor_kwargs,
    }
    var = await sensor.new_sensor(link_health_config)
    await cg.register_component(var, link_health_config)
    cg.add(var.set_parent(parent))
    cg.add(var.set_device_id(config[CONF_DEVICE_ID]))


async def create_companion_sensors(config, parent):
    """Create and register every auto-generated companion diagnostic sensor.

    Single to_code() entry point for the device-bound platforms (the counterpart of
    inject_companion_sensor_ids()), so adding a companion touches this module only:

    - Device Name: disabled by default (clutter control).
    - Last Result: the one enabled-by-default companion — it is the headline diagnostic
      value that turns a silently-ignored command into a self-explained one (e.g. a
      wind/rain lockout), so users should see it without an opt-in step.
    - RSSI / Last Seen / Exchange Failures: numeric link-health diagnostics, disabled by
      default (noise control). Last Seen publishes uptime-seconds, not a Home Assistant
      timestamp — the hub has no wall-clock time source; see IOHomeLastSeenSensor.
    """
    await _create_companion_text_sensor(
        config,
        parent,
        config[CONF_DEVICE_NAME_SENSOR_ID],
        _companion_sensor_name(config, "Device Name"),
        disabled_by_default=True,
    )
    await _create_companion_text_sensor(
        config,
        parent,
        config[CONF_LAST_RESULT_SENSOR_ID],
        _companion_sensor_name(config, "Last Result"),
        disabled_by_default=False,
    )
    await _create_link_health_sensor(
        config,
        parent,
        config[CONF_RSSI_SENSOR_ID],
        _companion_sensor_name(config, "RSSI"),
        **{
            CONF_UNIT_OF_MEASUREMENT: "dBm",
            CONF_DEVICE_CLASS: DEVICE_CLASS_SIGNAL_STRENGTH,
            CONF_STATE_CLASS: STATE_CLASS_MEASUREMENT,
            CONF_ACCURACY_DECIMALS: 0,
        },
    )
    await _create_link_health_sensor(
        config,
        parent,
        config[CONF_LAST_SEEN_SENSOR_ID],
        _companion_sensor_name(config, "Last Seen"),
        **{
            CONF_UNIT_OF_MEASUREMENT: "s",
            CONF_ACCURACY_DECIMALS: 0,
        },
    )
    await _create_link_health_sensor(
        config,
        parent,
        config[CONF_EXCHANGE_FAILURES_SENSOR_ID],
        _companion_sensor_name(config, "Exchange Failures"),
        **{
            CONF_STATE_CLASS: STATE_CLASS_TOTAL_INCREASING,
            CONF_ACCURACY_DECIMALS: 0,
        },
    )
