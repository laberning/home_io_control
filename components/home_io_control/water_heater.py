## @file
## @brief ESPHome water_heater platform schema and code generation.
## @ingroup hioc_codegen
##
## Binds an ESPHome water_heater entity to a domestic hot water heater (io_device_type: domestic_hot_water),
## with five opt-in companions: energy (kWh, for the energy dashboard), heat pump running, electric backup
## running, a heating-source text sensor and a boost switch. Boost and away travel in the heater's operating-mode word; both
## are confirmed on an Atlantic Calypso Connecté (issue #198). The heater is polled (default every five minutes);
## `status_poll_interval` sets the cadence, with the same one-minute floor as climate. Shared device-binding
## logic lives in platform_common.py; only the water heater's entity and companions are built here.
##
## `max_temperature` sets the target the heater's highest main parameter stands for (62 by default); it moves
## the card's range with it. `io_model: explorer_v4` makes it 70 unless the key says otherwise, and reads the
## heater's boost field the other way round (DHW_BOOST_INVERTED).
##
## The kind is required: the codegen checks it carries the WATER_HEATER capability, so a config can never
## send the water heater's register frames to another device.

import functools

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, sensor, switch, text_sensor, water_heater
from esphome.const import (
    CONF_ENERGY,
    CONF_ID,
    CONF_MAX_TEMPERATURE,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_RUNNING,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_KILOWATT_HOURS,
)

from . import home_io_control_ns
from .platform_common import (
    create_companion_sensors,
    create_user_companion,
    inject_companion_sensor_ids,
    platform_schema_extension,
    require_feature,
    validate_device_model,
    validate_register_poll_interval,
    warn_if_experimental_kind,
    wire_device_binding,
    CONF_DEVICE_TYPE,
    CONF_HOME_IO_CONTROL_ID,
    CONF_MODEL,
)

DEPENDENCIES = ["home_io_control"]

IOHomeWaterHeater = home_io_control_ns.class_("IOHomeWaterHeater", water_heater.WaterHeater, cg.Component)
IOHomeDhwEnergySensor = home_io_control_ns.class_("IOHomeDhwEnergySensor", sensor.Sensor, cg.Component)
IOHomeDhwBinarySensor = home_io_control_ns.class_(
    "IOHomeDhwBinarySensor", binary_sensor.BinarySensor, cg.Component
)
IOHomeDhwHeatingSourceSensor = home_io_control_ns.class_(
    "IOHomeDhwHeatingSourceSensor", text_sensor.TextSensor, cg.Component
)
IOHomeDhwBoostSwitch = home_io_control_ns.class_("IOHomeDhwBoostSwitch", switch.Switch, cg.Component)
DhwReading = IOHomeDhwBinarySensor.enum("Reading", is_class=True)

CONF_HEAT_PUMP_RUNNING = "heat_pump_running"
CONF_ELECTRIC_BACKUP_RUNNING = "electric_backup_running"
CONF_HEATING_SOURCE = "heating_source"
CONF_BOOST = "boost"

# (YAML key, IOHomeDhwBinarySensor::Reading) of each opt-in binary companion.
_BINARY_COMPANIONS = (
    (CONF_HEAT_PUMP_RUNNING, DhwReading.HEAT_PUMP),
    (CONF_ELECTRIC_BACKUP_RUNNING, DhwReading.ELECTRIC_BACKUP),
)


def _validate_water_heater_kind(config):
    """Require io_device_type, and a kind that is read and commanded as a water heater.

    The water heater's frames (family-0x00 register reads, the mode write, a target EXECUTE) go only to a
    device whose kind carries the WATER_HEATER capability, so the kind must be known at codegen time.
    """
    if CONF_DEVICE_TYPE not in config:
        raise cv.Invalid(
            "io_device_type is required on the water_heater platform: set io_device_type: domestic_hot_water",
            path=[CONF_DEVICE_TYPE],
        )
    require_feature(
        config,
        "the water_heater platform",
        True,
        required="WATER_HEATER",
        reason="it drives a domestic hot water heater (io_device_type: domestic_hot_water)",
    )
    return config


# The range the heater encodes its target over: main parameter 0 is DHW_TARGET_MIN_C, 0xC800 is the upper end.
# The upper end defaults to DHW_TARGET_MAX_C (proto_dhw.h), what a Calypso uses; an Explorer V4 uses 70.
DHW_TARGET_MIN_C = 20.0
DHW_TARGET_MAX_BOUND_C = 100.0
# The top of an Atlantic Explorer V4's scale, the default of max_temperature with io_model: explorer_v4.
EXPLORER_V4_TARGET_MAX_C = 70.0


def _validate_max_temperature(config):
    """Keep max_temperature above the fixed lower end and below boiling."""
    if CONF_MAX_TEMPERATURE not in config:
        return config
    if not DHW_TARGET_MIN_C < config[CONF_MAX_TEMPERATURE] <= DHW_TARGET_MAX_BOUND_C:
        raise cv.Invalid(
            f"max_temperature must be above {DHW_TARGET_MIN_C:g} and at most {DHW_TARGET_MAX_BOUND_C:g} "
            "degrees Celsius: it is the target the heater's highest main parameter stands for",
            path=[CONF_MAX_TEMPERATURE],
        )
    return config


def _inject_companion_ids(config):
    """Declare the auto-generated companion IDs during validation (ESPHome 2026.x StaticVector sizing).

    The opt-in companions (sensors and the boost switch) are schemas of their own, whose IDs are declared by
    their GenerateID.
    """
    return inject_companion_sensor_ids(config, CONF_ID)


CONFIG_SCHEMA = cv.All(
    water_heater.water_heater_schema(IOHomeWaterHeater)
    .extend(platform_schema_extension())
    .extend(
        {
            cv.Optional(CONF_MAX_TEMPERATURE): cv.temperature,
            cv.Optional(CONF_ENERGY): sensor.sensor_schema(
                IOHomeDhwEnergySensor,
                unit_of_measurement=UNIT_KILOWATT_HOURS,
                accuracy_decimals=3,
                device_class=DEVICE_CLASS_ENERGY,
                state_class=STATE_CLASS_TOTAL_INCREASING,
            ).extend(cv.COMPONENT_SCHEMA),
            cv.Optional(CONF_HEAT_PUMP_RUNNING): binary_sensor.binary_sensor_schema(
                IOHomeDhwBinarySensor, device_class=DEVICE_CLASS_RUNNING
            ).extend(cv.COMPONENT_SCHEMA),
            cv.Optional(CONF_ELECTRIC_BACKUP_RUNNING): binary_sensor.binary_sensor_schema(
                IOHomeDhwBinarySensor, device_class=DEVICE_CLASS_RUNNING
            ).extend(cv.COMPONENT_SCHEMA),
            cv.Optional(CONF_HEATING_SOURCE): text_sensor.text_sensor_schema(IOHomeDhwHeatingSourceSensor).extend(
                cv.COMPONENT_SCHEMA
            ),
            # Restore mode DISABLED: the switch shows what the heater reports and never writes at boot, so a
            # reboot cannot end a running boost.
            cv.Optional(CONF_BOOST): switch.switch_schema(
                IOHomeDhwBoostSwitch, default_restore_mode="DISABLED"
            ).extend(cv.COMPONENT_SCHEMA),
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    _validate_water_heater_kind,
    functools.partial(validate_register_poll_interval, platform="water_heater"),
    _validate_max_temperature,
    validate_device_model,
    warn_if_experimental_kind,
    _inject_companion_ids,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await water_heater.register_water_heater(var, config)

    parent = await cg.get_variable(config[CONF_HOME_IO_CONTROL_ID])
    await wire_device_binding(var, parent, config)
    await create_companion_sensors(config, parent)

    max_temperature = config.get(CONF_MAX_TEMPERATURE)
    if max_temperature is None and config.get(CONF_MODEL) == "explorer_v4":
        max_temperature = EXPLORER_V4_TARGET_MAX_C
    if max_temperature is not None:
        cg.add(var.set_target_range_max(max_temperature))

    if CONF_ENERGY in config:
        await create_user_companion(
            config, parent, new_entity=sensor.new_sensor, entity_config=config[CONF_ENERGY]
        )

    for key, reading in _BINARY_COMPANIONS:
        if key not in config:
            continue
        companion = await create_user_companion(
            config, parent, new_entity=binary_sensor.new_binary_sensor, entity_config=config[key]
        )
        cg.add(companion.set_reading(reading))

    if CONF_HEATING_SOURCE in config:
        await create_user_companion(
            config,
            parent,
            new_entity=text_sensor.new_text_sensor,
            entity_config=config[CONF_HEATING_SOURCE],
        )

    if CONF_BOOST in config:
        await create_user_companion(
            config, parent, new_entity=switch.new_switch, entity_config=config[CONF_BOOST]
        )
