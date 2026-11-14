## @file
## @brief ESPHome water_heater platform schema and code generation.
## @ingroup hioc_codegen
##
## Binds an ESPHome water_heater entity to a domestic hot water heater (io_device_type: domestic_hot_water),
## with four opt-in companions: energy (kWh, for the energy dashboard), heat pump running, electric backup
## running, and a boost switch. Boost and away travel in the heater's operating-mode word and are
## experimental: not confirmed on hardware yet. The heater is polled (default every five minutes);
## `status_poll_interval` sets the cadence, with the same one-minute floor as climate. Shared device-binding
## logic lives in platform_common.py; only the water heater's entity and companions are built here.
##
## The kind is required: the codegen checks it carries the WATER_HEATER capability, so a config can never
## send the water heater's register frames to another device.

import functools

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, sensor, switch, water_heater
from esphome.const import (
    CONF_DEVICE_ID,
    CONF_ENERGY,
    CONF_ID,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_RUNNING,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_KILOWATT_HOURS,
)

from . import home_io_control_ns
from .hub_validators import FEATURE_WATER_HEATER
from .platform_common import (
    create_companion_sensors,
    inherit_esphome_device,
    inject_companion_sensor_ids,
    platform_schema_extension,
    require_feature,
    validate_device_model,
    validate_register_poll_interval,
    warn_if_experimental_kind,
    wire_device_binding,
    CONF_DEVICE_TYPE,
    CONF_HOME_IO_CONTROL_ID,
    CONF_IO_DEVICE_ID,
)

DEPENDENCIES = ["home_io_control"]

IOHomeWaterHeater = home_io_control_ns.class_("IOHomeWaterHeater", water_heater.WaterHeater, cg.Component)
IOHomeDhwEnergySensor = home_io_control_ns.class_("IOHomeDhwEnergySensor", sensor.Sensor, cg.Component)
IOHomeDhwBinarySensor = home_io_control_ns.class_(
    "IOHomeDhwBinarySensor", binary_sensor.BinarySensor, cg.Component
)
IOHomeDhwBoostSwitch = home_io_control_ns.class_("IOHomeDhwBoostSwitch", switch.Switch, cg.Component)
DhwReading = IOHomeDhwBinarySensor.enum("Reading", is_class=True)

CONF_HEAT_PUMP_RUNNING = "heat_pump_running"
CONF_ELECTRIC_BACKUP_RUNNING = "electric_backup_running"
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
        required=FEATURE_WATER_HEATER,
        reason="it drives a domestic hot water heater (io_device_type: domestic_hot_water)",
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
    validate_device_model,
    warn_if_experimental_kind,
    _inject_companion_ids,
)


def _companion_config(companion, config):
    """A companion's config, grouped under the entity's ESPHome sub-device unless it names its own."""
    if CONF_DEVICE_ID not in companion:
        inherit_esphome_device(companion, config)
    return companion


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await water_heater.register_water_heater(var, config)

    parent = await cg.get_variable(config[CONF_HOME_IO_CONTROL_ID])
    await wire_device_binding(var, parent, config)
    await create_companion_sensors(config, parent)

    if CONF_ENERGY in config:
        energy_config = _companion_config(config[CONF_ENERGY], config)
        energy = await sensor.new_sensor(energy_config)
        await cg.register_component(energy, energy_config)
        cg.add(energy.set_parent(parent))
        cg.add(energy.set_device_id(config[CONF_IO_DEVICE_ID]))

    for key, reading in _BINARY_COMPANIONS:
        if key not in config:
            continue
        companion_config = _companion_config(config[key], config)
        companion = await binary_sensor.new_binary_sensor(companion_config)
        await cg.register_component(companion, companion_config)
        cg.add(companion.set_parent(parent))
        cg.add(companion.set_device_id(config[CONF_IO_DEVICE_ID]))
        cg.add(companion.set_reading(reading))

    if CONF_BOOST in config:
        boost_config = _companion_config(config[CONF_BOOST], config)
        boost = await switch.new_switch(boost_config)
        await cg.register_component(boost, boost_config)
        cg.add(boost.set_parent(parent))
        cg.add(boost.set_device_id(config[CONF_IO_DEVICE_ID]))
