## @file
## @brief Deprecated `button:` platform for the pairing button -- use
## `home_io_control.discover_and_pair_button: true` instead.
## @ingroup hioc_codegen
##
## Exposes the same Home Assistant button entity and companion "Last Pairing Result" diagnostic
## text sensor that `home_io_control.discover_and_pair_button: true` now creates directly (see
## __init__.py's `_create_discover_and_pair_button()`). Kept working, with a deprecation warning,
## for configs that predate that flag -- see `_warn_deprecated_platform()` below and ADR 0036.

import logging

import esphome.codegen as cg
from esphome.components import button, text_sensor
import esphome.config_validation as cv
import esphome.final_validate as fv
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
    IOHomeControlComponent,
    IOHomeDiscoverButton,
    IOHomePairingResultTextSensor,
    CONF_DISCOVER_AND_PAIR_BUTTON,
    CONF_HOME_IO_CONTROL_ID,
    inherit_esphome_device,
)

_LOGGER = logging.getLogger(__name__)

DEPENDENCIES = ["home_io_control"]

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


def _warn_deprecated_platform(config):
    """Warn that this platform is superseded by home_io_control.discover_and_pair_button.

    Placed FIRST in CONFIG_SCHEMA's cv.All() chain so the warning still reaches a user whose entry
    also has an unrelated validation error. ESPHome runs each entry's schema exactly once per
    `esphome config|compile|run`, so this fires once per legacy entry per invocation -- no dedupe
    flag needed. A validator rather than to_code()'s first line (ESPHome's own precedent for a
    whole-component deprecation) so `esphome config` and the dashboard's validate-only pass show it
    too, and to match this component's existing convention of warning from validators.
    """
    _LOGGER.warning(
        "The 'home_io_control' button platform is deprecated. Set "
        "'discover_and_pair_button: true' in the 'home_io_control:' block instead and delete this "
        "'button:' entry; the hub then creates the 'Discover & Pair' button and its 'Last Pairing "
        "Result' sensor itself. This entry still works, but will become a hard error in a future "
        "release."
    )
    return config


CONFIG_SCHEMA = cv.All(
    _warn_deprecated_platform,
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


def _reject_duplicate_with_hub_flag(config):
    """Reject having both this legacy entry and the hub's own discover_and_pair_button: true.

    Rejected unconditionally, regardless of this entry's own name/device_id -- by default both
    create a button named "Discover & Pair" on the same device, which ESPHome's entity-duplicate
    validator refuses, but it only notices inside the hub's to_code(), where the resulting
    cv.Invalid surfaces as an uncaught traceback rather than a validation message. A renamed or
    re-homed entry could in principle coexist without colliding, but forcing the choice here keeps
    the guard simple and the migration unambiguous: one pairing trigger, not "two unless you were
    careful". Platform modules get their own FINAL_VALIDATE_SCHEMA slot, so this does not touch
    __init__.py's (already used for address-collision detection).
    """
    hub_config = fv.full_config.get().get("home_io_control") or {}
    if isinstance(hub_config, dict) and hub_config.get(CONF_DISCOVER_AND_PAIR_BUTTON):
        raise cv.Invalid(
            "Both 'home_io_control.discover_and_pair_button: true' and a legacy "
            "'button: - platform: home_io_control' entry are configured. Delete this 'button:' "
            "entry -- the hub flag replaces it and creates the same button and sensor itself."
        )
    return config


FINAL_VALIDATE_SCHEMA = _reject_duplicate_with_hub_flag


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
