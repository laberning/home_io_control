## @file
## @brief ESPHome climate platform schema and code generation.
## @ingroup hioc_codegen
##
## Defines the experimental IO-Homecontrol heating/climate integration and wires it to the shared
## hub. Heaters with a window sensor also get a Window binary sensor and an Open Window Detection
## switch. The heater is polled (default every five minutes); `status_poll_interval` sets the cadence. Shared device-binding logic lives in platform_common.py; only climate-specific entity
## construction/registration stays here.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, climate, switch
from esphome.const import (
    CONF_DISABLED_BY_DEFAULT,
    CONF_ID,
    CONF_NAME,
    DEVICE_CLASS_WINDOW,
    ENTITY_CATEGORY_CONFIG,
)
from esphome.core import ID

from . import home_io_control_ns
from .hub_validators import heating_profile_has_window
from .platform_common import (
    companion_entity_name,
    companion_id_base,
    create_companion_sensors,
    inherit_esphome_device,
    inject_companion_sensor_ids,
    platform_schema_extension,
    wire_device_binding,
    CONF_DEVICE_TYPE,
    CONF_HOME_IO_CONTROL_ID,
    CONF_IO_DEVICE_ID,
    CONF_STATUS_POLL_INTERVAL,
    CONF_SUBTYPE,
)

DEPENDENCIES = ["home_io_control"]

IOHomeClimate = home_io_control_ns.class_("IOHomeClimate", climate.Climate, cg.Component)
IOHomeWindowBinarySensor = home_io_control_ns.class_(
    "IOHomeWindowBinarySensor", binary_sensor.BinarySensor, cg.Component
)
IOHomeOpenWindowDetectionSwitch = home_io_control_ns.class_(
    "IOHomeOpenWindowDetectionSwitch", switch.Switch, cg.Component
)

# Internal config keys for the heater-only companion IDs (injected by the post-validator).
CONF_WINDOW_SENSOR_ID = "_window_sensor_id"
CONF_WINDOW_DETECTION_SWITCH_ID = "_window_detection_switch_id"


def _has_window_companions(config):
    """True when the declared heater kind has a window sensor and a detection switch.

    The kind is the declared io_device_type/io_subtype (subtype 0 when omitted), the same key the
    C++ heating profile uses. A heater declared without a type gets no companions: nothing at
    validation time says what it is.
    """
    return CONF_DEVICE_TYPE in config and heating_profile_has_window(
        config[CONF_DEVICE_TYPE], config.get(CONF_SUBTYPE, 0)
    )


def _inject_companion_ids(config):
    """Declare the heater companion IDs during validation (ESPHome 2026.x StaticVector sizing)."""
    if _has_window_companions(config):
        base = companion_id_base(config, CONF_ID)
        config[CONF_WINDOW_SENSOR_ID] = ID(
            f"{base}_window_sensor", is_declaration=True, type=IOHomeWindowBinarySensor
        )
        config[CONF_WINDOW_DETECTION_SWITCH_ID] = ID(
            f"{base}_window_detection_switch",
            is_declaration=True,
            type=IOHomeOpenWindowDetectionSwitch,
        )
    return inject_companion_sensor_ids(config, CONF_ID)


# A heater's state moves over minutes and a poll costs one exchange per register, so the interval floor is
# well above the cover floor (hub_names.MIN_STATUS_POLL_INTERVAL_MS).
MIN_CLIMATE_POLL_INTERVAL_MS = 60000


def _validate_climate_poll_interval(config):
    # The key is accepted by the shared platform schema; a climate entity raises its floor to one
    # minute. Without it the hub polls every five minutes.
    interval = config.get(CONF_STATUS_POLL_INTERVAL)
    if interval is not None and interval.total_milliseconds < MIN_CLIMATE_POLL_INTERVAL_MS:
        raise cv.Invalid(
            f"status_poll_interval must be at least {MIN_CLIMATE_POLL_INTERVAL_MS}ms for the "
            "climate platform: a heater is polled with one radio exchange per register",
            path=[CONF_STATUS_POLL_INTERVAL],
        )
    return config


CONFIG_SCHEMA = cv.All(
    climate.climate_schema(IOHomeClimate)
    .extend(platform_schema_extension())
    .extend(cv.COMPONENT_SCHEMA),
    _validate_climate_poll_interval,
    _inject_companion_ids,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await climate.register_climate(var, config)

    parent = await cg.get_variable(config[CONF_HOME_IO_CONTROL_ID])
    await wire_device_binding(var, parent, config)
    await create_companion_sensors(config, parent)

    if CONF_WINDOW_SENSOR_ID in config:
        window_config = binary_sensor.binary_sensor_schema(
            IOHomeWindowBinarySensor, device_class=DEVICE_CLASS_WINDOW
        ).extend(cv.COMPONENT_SCHEMA)(
            inherit_esphome_device(
                {
                    CONF_ID: config[CONF_WINDOW_SENSOR_ID],
                    CONF_NAME: companion_entity_name(config, "Window"),
                },
                config,
            )
        )
        window_sensor = await binary_sensor.new_binary_sensor(window_config)
        await cg.register_component(window_sensor, window_config)
        cg.add(window_sensor.set_parent(parent))
        cg.add(window_sensor.set_device_id(config[CONF_IO_DEVICE_ID]))

    if CONF_WINDOW_DETECTION_SWITCH_ID in config:
        # Restore mode DISABLED: the switch shows what the heater reports, never a restored guess.
        detection_config = switch.switch_schema(
            IOHomeOpenWindowDetectionSwitch,
            default_restore_mode="DISABLED",
            entity_category=ENTITY_CATEGORY_CONFIG,
        ).extend(cv.COMPONENT_SCHEMA)(
            inherit_esphome_device(
                {
                    CONF_ID: config[CONF_WINDOW_DETECTION_SWITCH_ID],
                    CONF_NAME: companion_entity_name(config, "Open Window Detection"),
                    CONF_DISABLED_BY_DEFAULT: True,
                },
                config,
            )
        )
        detection_switch = await switch.new_switch(detection_config)
        await cg.register_component(detection_switch, detection_config)
        cg.add(detection_switch.set_parent(parent))
        cg.add(detection_switch.set_device_id(config[CONF_IO_DEVICE_ID]))
