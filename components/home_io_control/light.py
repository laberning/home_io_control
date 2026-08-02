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
from esphome.const import CONF_DEFAULT_TRANSITION_LENGTH, CONF_OUTPUT_ID

from . import home_io_control_ns
from .platform_common import (
    create_companion_sensors,
    inject_companion_sensor_ids,
    platform_schema_extension,
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
        # ESPHome's light_schema() default (1s) client-side-fades every brightness change by
        # repeatedly calling write_state() over that window (LightState::loop()'s transformer).
        # Each of those calls is a real radio command over IO-Homecontrol's ~300ms round trip —
        # unlike a PWM/LED output, write_state() here is not cheap, and the resulting stream of
        # stale/superseding commands (plus each device reply re-triggering on_device_update_(),
        # which performs another call and can restart the transition) was observed on real
        # hardware to loop indefinitely. Default to instant application instead; still overridable
        # in YAML by anyone who understands the tradeoff.
        schema = schema.extend(
            {cv.Optional(CONF_DEFAULT_TRANSITION_LENGTH, default="0s"): cv.positive_time_period_milliseconds}
        )
    return schema(config)


CONFIG_SCHEMA = cv.All(_validate, _inject_companion_ids)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    await cg.register_component(var, config)
    await light.register_light(var, config)

    cg.add(var.set_dimmable(config[CONF_DIMMABLE]))

    parent = await cg.get_variable(config[CONF_HOME_IO_CONTROL_ID])
    await wire_device_binding(var, parent, config)
    await create_companion_sensors(config, parent)
