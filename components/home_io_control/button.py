## @file
## @brief ESPHome pairing-button schema and code generation.
## @ingroup hioc_codegen
##
## Exposes the Home Assistant button entity that starts device discovery and pairing, plus
## its companion "Last Pairing Result" diagnostic text sensor (machine-readable telemetry
## summary, published after every attempt).

import esphome.codegen as cg
from esphome.components import button, text_sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_DISABLED_BY_DEFAULT,
    CONF_ENTITY_CATEGORY,
    CONF_ID,
    CONF_NAME,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
)
from esphome.core import ID

from . import (
    home_io_control_ns,
    IOHomeControlComponent,
    CONF_HOME_IO_CONTROL_ID,
    inherit_esphome_device,
)

DEPENDENCIES = ["home_io_control"]

IOHomeDiscoverButton = home_io_control_ns.class_(
    "IOHomeDiscoverButton", button.Button, cg.Component
)
IOHomePairingResultTextSensor = home_io_control_ns.class_(
    "IOHomePairingResultTextSensor", text_sensor.TextSensor, cg.Component
)

# Internal config key for the companion pairing-result sensor ID (injected by post-validator).
CONF_PAIRING_RESULT_SENSOR_ID = "_pairing_result_sensor_id"


def _inject_pairing_result_sensor_id(config):
    """Declare the companion pairing-result sensor ID during schema validation.

    ESPHome 2026.x sizes its runtime component vector from the number of component IDs
    known at the end of schema validation — before to_code() runs. A companion entity
    created only in to_code() is not counted and silently drops at runtime. See the
    identical pattern (and its full rationale) in platform_common.py::companion_id_base().
    """
    from esphome.helpers import sanitize

    parent_id = config[CONF_ID]
    base = parent_id.id if parent_id.id else sanitize(config.get(CONF_NAME, "")).lower()
    config[CONF_PAIRING_RESULT_SENSOR_ID] = ID(
        f"{base}_pairing_result_sensor",
        is_declaration=True,
        type=IOHomePairingResultTextSensor,
    )
    return config


CONFIG_SCHEMA = cv.All(
    button.button_schema(
        IOHomeDiscoverButton,
        entity_category=ENTITY_CATEGORY_CONFIG,
    )
    .extend(
        {
            cv.GenerateID(CONF_HOME_IO_CONTROL_ID): cv.use_id(
                IOHomeControlComponent
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    _inject_pairing_result_sensor_id,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await button.register_button(var, config)

    parent = await cg.get_variable(config[CONF_HOME_IO_CONTROL_ID])
    cg.add(var.set_parent(parent))

    result_sensor_config = inherit_esphome_device(
        {
            CONF_ID: config[CONF_PAIRING_RESULT_SENSOR_ID],
            CONF_NAME: "Last Pairing Result",
            CONF_DISABLED_BY_DEFAULT: False,
            CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_DIAGNOSTIC,
        },
        config,
    )
    result_sensor = await text_sensor.new_text_sensor(result_sensor_config)
    await cg.register_component(result_sensor, result_sensor_config)
    cg.add(result_sensor.set_parent(parent))
