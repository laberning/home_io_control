## @file
## @brief ESPHome binary/dimmable light platform schema and code generation.
## @ingroup hioc_codegen
##
## Defines the binary (default) or dimmable light integration and wires it to the shared hub.
## Shared device-binding logic lives in platform_common.py; only light-specific entity
## construction/registration stays here.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light
from esphome.const import CONF_OUTPUT_ID

from . import home_io_control_ns
from .hub_validators import FEATURE_MAIN_PARAMETER_ONLY
from .platform_common import (
    INSTANT_LIGHT_TRANSITION,
    create_companion_sensors,
    inject_companion_sensor_ids,
    platform_schema_extension,
    require_feature,
    validate_device_model,
    warn_if_experimental_kind,
    wire_device_binding,
    CONF_HOME_IO_CONTROL_ID,
)

DEPENDENCIES = ["home_io_control"]

IOHomeLight = home_io_control_ns.class_("IOHomeLight", light.LightOutput, cg.Component)

# Opt-in: the protocol gives no machine-readable "is this dimmable" signal (DISCOVER_RESP's
# subtype byte is manufacturer-opaque), so this defaults to false/binary rather than guessing.
CONF_DIMMABLE = "dimmable"


def _inject_companion_ids(config):
    # Light reads its entity ID from CONF_OUTPUT_ID rather than CONF_ID.
    return inject_companion_sensor_ids(config, CONF_OUTPUT_ID)


def _require_dimmable_kind(config):
    """Reject `dimmable: true` on a declared light kind that is on/off only (subtype 58)."""
    require_feature(
        config,
        CONF_DIMMABLE,
        config[CONF_DIMMABLE],
        forbidden=FEATURE_MAIN_PARAMETER_ONLY,
        reason="this light is on/off only (KLF 200 Appendix 2); remove dimmable:",
    )
    return config


def _validate(config):
    # The LightType passed to light.light_schema() determines which YAML keys are even accepted
    # (e.g. gamma_correct only makes sense for a brightness-capable light), so it must be chosen
    # from the raw dimmable: value before the rest of the schema runs.
    light_type = (
        light.LightType.BRIGHTNESS_ONLY
        if cv.boolean(config.get(CONF_DIMMABLE, False))
        else light.LightType.BINARY
    )
    schema = (
        light.light_schema(IOHomeLight, light_type)
        .extend(platform_schema_extension())
        .extend({cv.Optional(CONF_DIMMABLE, default=False): cv.boolean})
        .extend(cv.COMPONENT_SCHEMA)
    )
    if light_type == light.LightType.BRIGHTNESS_ONLY:
        schema = schema.extend(INSTANT_LIGHT_TRANSITION)
    return schema(config)


CONFIG_SCHEMA = cv.All(
    _validate,
    validate_device_model,
    _require_dimmable_kind,
    warn_if_experimental_kind,
    _inject_companion_ids,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    await cg.register_component(var, config)
    await light.register_light(var, config)

    cg.add(var.set_dimmable(config[CONF_DIMMABLE]))

    parent = await cg.get_variable(config[CONF_HOME_IO_CONTROL_ID])
    await wire_device_binding(var, parent, config)
    await create_companion_sensors(config, parent)
