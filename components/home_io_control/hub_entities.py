## @file
## @brief Hub-level entities created from boolean flags in the ``home_io_control:`` block.
## @ingroup hioc_codegen
##
## Declares their IDs during validation (ADR 0009: an ID first created in to_code() is
## silently dropped at runtime) and creates them in to_code(): the foreign-pairing and
## 1W-key-recovery arming switches, the Scan Paired Devices button, and the Discover &
## Pair button with its Last Pairing Result sensor.

import esphome.codegen as cg
import esphome.config_validation as cv
# Aliased so they cannot be mistaken for this package's own platform modules (see hub_names.py).
from esphome.components import button as button_component
from esphome.components import switch as switch_component
from esphome.components import text_sensor as text_sensor_component
from esphome.const import (
    CONF_ID,
    CONF_NAME,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
)
from esphome.core import ID

from .hub_names import (
    CONF_ACCEPT_FOREIGN_PAIRING,
    CONF_ACCEPT_FOREIGN_PAIRING_SWITCH_ID,
    CONF_DISCOVER_AND_PAIR_BUTTON,
    CONF_DISCOVER_AND_PAIR_BUTTON_ID,
    CONF_DISCOVER_AND_PAIR_RESULT_SENSOR_ID,
    CONF_RECOVER_ONEWAY_KEY,
    CONF_RECOVER_ONEWAY_KEY_SWITCH_ID,
    CONF_SCAN_PAIRED_DEVICES_BUTTON,
    CONF_SCAN_PAIRED_DEVICES_BUTTON_ID,
    IOHomeAcceptForeignPairingSwitch,
    IOHomeDiscoverButton,
    IOHomePairingResultTextSensor,
    IOHomeRecoverOneWayKeySwitch,
    IOHomeScanPairedDevicesButton,
)


def _inject_hub_entity_id(config, *, flag_key, id_key, suffix, cls):
    """Shared body for the hub-level entities gated by a bare boolean flag in the
    `home_io_control:` block (accept_foreign_pairing, recover_oneway_key,
    scan_paired_devices_button, discover_and_pair_button): declare the entity's ID during
    validation, under the `{hub_id}_{suffix}` name, only when its flag is set. See
    companion_id_base() in platform_common.py for why this must happen at validation time rather
    than in to_code().
    """
    if not config[flag_key]:
        return config
    parent_id = config[CONF_ID]
    base = parent_id.id if parent_id.id else "home_io_control"
    config[id_key] = ID(f"{base}_{suffix}", is_declaration=True, type=cls)
    return config


def inject_accept_foreign_pairing_switch_id(config):
    return _inject_hub_entity_id(
        config,
        flag_key=CONF_ACCEPT_FOREIGN_PAIRING,
        id_key=CONF_ACCEPT_FOREIGN_PAIRING_SWITCH_ID,
        suffix="accept_foreign_pairing_switch",
        cls=IOHomeAcceptForeignPairingSwitch,
    )


def inject_recover_oneway_key_switch_id(config):
    return _inject_hub_entity_id(
        config,
        flag_key=CONF_RECOVER_ONEWAY_KEY,
        id_key=CONF_RECOVER_ONEWAY_KEY_SWITCH_ID,
        suffix="recover_oneway_key_switch",
        cls=IOHomeRecoverOneWayKeySwitch,
    )


def inject_scan_paired_devices_button_id(config):
    return _inject_hub_entity_id(
        config,
        flag_key=CONF_SCAN_PAIRED_DEVICES_BUTTON,
        id_key=CONF_SCAN_PAIRED_DEVICES_BUTTON_ID,
        suffix="scan_paired_devices_button",
        cls=IOHomeScanPairedDevicesButton,
    )


def inject_discover_and_pair_button_id(config):
    return _inject_hub_entity_id(
        config,
        flag_key=CONF_DISCOVER_AND_PAIR_BUTTON,
        id_key=CONF_DISCOVER_AND_PAIR_BUTTON_ID,
        suffix="discover_and_pair_button",
        cls=IOHomeDiscoverButton,
    )


def inject_discover_and_pair_result_sensor_id(config):
    """Second ID off the same flag: the button always ships with its "Last Pairing Result" sensor,
    so both IDs are gated on CONF_DISCOVER_AND_PAIR_BUTTON. _inject_hub_entity_id() already no-ops
    when the flag is false, so no extra guard is needed here.
    """
    return _inject_hub_entity_id(
        config,
        flag_key=CONF_DISCOVER_AND_PAIR_BUTTON,
        id_key=CONF_DISCOVER_AND_PAIR_RESULT_SENSOR_ID,
        suffix="pairing_result_sensor",
        cls=IOHomePairingResultTextSensor,
    )


async def _create_hub_entity(schema, new_entity, entity_id, name, var):
    """Create one hub-level entity from its declared ID and a fixed name, bound to the hub.

    A bare {id, name} dict is run through the platform's entity schema plus COMPONENT_SCHEMA, so
    it carries the entity/component defaults new_*()/register_component() require, instead of a
    hand-assembled config dict of its own (tuning.py's _create_number()/_create_select() do the
    same for the tuning entities).
    """
    entity_config = schema.extend(cv.COMPONENT_SCHEMA)({CONF_ID: entity_id, CONF_NAME: name})
    entity = await new_entity(entity_config)
    await cg.register_component(entity, entity_config)
    cg.add(entity.set_parent(var))
    return entity


async def create_hub_arming_switch(config, var, *, cls, id_key, name):
    """Create a hub-level arming switch (key extraction or key adoption).

    ALWAYS_OFF is a security property, not a UX default: every switch built here arms a window
    (foreign-key extraction or 1W key adoption) that must never come back armed after a reboot.
    """
    schema = switch_component.switch_schema(
        cls,
        default_restore_mode="ALWAYS_OFF",  # never auto-arm after a reboot
        entity_category=ENTITY_CATEGORY_CONFIG,
    )
    await _create_hub_entity(schema, switch_component.new_switch, config[id_key], name, var)


async def create_scan_paired_devices_button(config, var):
    """Create the hub-level "Scan Paired Devices" button.

    The `scan_paired_devices` native API action is registered independently in C++
    (ManagementActions::register_actions()) and is unaffected by this key -- the button is an extra
    trigger onto the same method, not a replacement.
    """
    schema = button_component.button_schema(IOHomeScanPairedDevicesButton, entity_category=ENTITY_CATEGORY_CONFIG)
    await _create_hub_entity(
        schema, button_component.new_button, config[CONF_SCAN_PAIRED_DEVICES_BUTTON_ID], "Scan Paired Devices", var
    )


async def create_discover_and_pair_button(config, var):
    """Create the hub-level "Discover & Pair" button and its "Last Pairing Result" sensor.

    The two are always created together: the sensor is the only place a pairing attempt's
    machine-readable outcome ever appears, so a button without it would be a button whose result
    you cannot read.

    inherit_esphome_device() is deliberately NOT called: the hub's own config has no `device_id:`
    slot for these to inherit -- see that function's docstring. This is the one behaviour the
    deprecated `button:` platform (button.py) had that this flag form cannot reproduce.
    """
    button_schema = button_component.button_schema(IOHomeDiscoverButton, entity_category=ENTITY_CATEGORY_CONFIG)
    await _create_hub_entity(
        button_schema, button_component.new_button, config[CONF_DISCOVER_AND_PAIR_BUTTON_ID], "Discover & Pair", var
    )
    sensor_schema = text_sensor_component.text_sensor_schema(
        IOHomePairingResultTextSensor, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    )
    await _create_hub_entity(
        sensor_schema,
        text_sensor_component.new_text_sensor,
        config[CONF_DISCOVER_AND_PAIR_RESULT_SENSOR_ID],
        "Last Pairing Result",
        var,
    )
