## @file
## @brief Checks that need the whole YAML: every device declared once, and no hub address used twice.
## @ingroup hioc_codegen
##
## FINAL_VALIDATE_SCHEMA of the hub block (`final_validate()`). Only final validation sees the entries of the
## device-bound platforms next to `home_io_control:`; each platform entry is validated on its own before that.

import esphome.config_validation as cv
import esphome.final_validate as fv

from .hub_names import CONF_IO_DEVICE_ID
from .hub_validators import declared_device_entries
from .oneway_controllers import final_validate_oneway_controller_addresses


def final_validate_one_main_entity_per_device(config):
    """Reject an `io_device_id:` that two main entities declare.

    A device is declared by one main entity (a cover, a light, a climate, a `sensor:` entry, ...); a second
    declaration would register the same device twice with possibly different kinds, of which the hub keeps the
    first and drops the other silently. The message names both entries.
    """
    declared = {}
    for domain, entry, owner_name in declared_device_entries(fv.full_config.get()):
        device_id = entry.get(CONF_IO_DEVICE_ID)
        owner = f"{domain} '{owner_name}'"
        if device_id in declared:
            raise cv.Invalid(
                f"io_device_id {device_id} is declared by {declared[device_id]} and by {owner}: "
                "a device is declared by one main entity; give the other entity's readings to "
                "the first (its reading keys), or use another device"
            )
        if device_id:
            declared[device_id] = owner
    return config


def final_validate(config):
    """Every check across the whole YAML, in the order they are reported."""
    config = final_validate_one_main_entity_per_device(config)
    return final_validate_oneway_controller_addresses(config)
