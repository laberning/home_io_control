## @file
## @brief Retired `button:` platform: a `button:` entry with `platform: home_io_control` is a
## config error that names the replacement.
## @ingroup hioc_codegen
##
## The pairing button and its "Last Pairing Result" sensor come from the hub block's
## `discover_and_pair_button: true` (hub_entities.py, `create_discover_and_pair_button()`); every
## hub-level entity is created from a hub flag, never from a platform entry (ADR 0036). This module
## stays so the old entry fails with this message instead of ESPHome's generic "platform not found".

import esphome.config_validation as cv

CONFIG_SCHEMA = cv.invalid(
    "The 'home_io_control' button platform has been removed. Delete this 'button:' entry and set "
    "'discover_and_pair_button: true' in the 'home_io_control:' block; the hub then creates the "
    "'Discover & Pair' button and its 'Last Pairing Result' sensor itself."
)
