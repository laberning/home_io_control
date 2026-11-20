#!/usr/bin/env python3
"""Behavioural tests for the Home IO Control ESPHome codegen: what a YAML config is accepted or
rejected with, and the IDs validation declares.

Stdlib ``assert``-based, no pytest -- the same dependency-light convention as
``scripts/corpus/tests/run_tests.py``. It must run inside the ESPHome container, because the
modules under test import ``esphome.*``; ``make py-test`` does that, with the repo mounted
read-only so nothing can be written into the tree.

Three parts:

- **Unit cases** call the validators and ID injectors directly, with the component imported as
  ``esphome.components.home_io_control`` -- the name ESPHome itself gives an external component.
- **Fixture cases** run ``esphome config`` on every YAML under ``fixtures/``. A file whose first
  line is ``# expect: valid`` must validate; one starting ``# expect-error: <text>`` must fail
  with ``<text>`` in the output. These cover rejections that need a whole config (packages,
  final validation across components).
- **Coverage gate**: every function in ``components/home_io_control/*.py`` that raises
  ``cv.Invalid`` must be named by at least one case's ``covers``. A new validator therefore
  arrives with a test that shows its message, or this runner fails and names it.
"""

import ast
import asyncio
import os
import subprocess
import sys
import traceback
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
COMPONENT_DIR = REPO / "components" / "home_io_control"
FIXTURES = Path(__file__).resolve().parent / "fixtures"

# (name, function, covers)
UNIT_CASES = []


def case(*covers):
    """Register a unit case; `covers` names the `<module>.<function>` rejection paths it reaches."""

    def register(fn):
        UNIT_CASES.append((fn.__name__, fn, covers))
        return fn

    return register


def import_component():
    """Import the component the way ESPHome resolves an external component."""
    import esphome.components

    if str(COMPONENT_DIR.parent) not in esphome.components.__path__:
        esphome.components.__path__.append(str(COMPONENT_DIR.parent))
    import esphome.components.home_io_control as package

    return package


def climate_reading_keys(config):
    """platform_common.validate_reading_keys() for a climate entry, the platform the capability cases are about."""
    from esphome.components.home_io_control import platform_common as pc

    return pc.validate_reading_keys(config, platform=pc.CLIMATE_PLATFORM)


def expect_invalid(fn, value, fragment):
    """`fn(value)` raises cv.Invalid whose message contains `fragment`."""
    import esphome.config_validation as cv

    try:
        fn(value)
    except cv.Invalid as err:
        assert fragment in str(err), f"expected {fragment!r} in {str(err)!r}"
        return
    raise AssertionError(f"{fn.__name__}({value!r}) was accepted; expected rejection containing {fragment!r}")


# =============================================================================================
# Unit cases
# =============================================================================================


@case("tuning._hex_value")
def key_extraction_bytes_are_hex():
    import_component()
    from esphome.components.home_io_control import tuning as t

    multi_info = t._hex_value("key_extraction_multi_info", 2)
    assert multi_info(0xDC) == 0xDC
    assert multi_info("0xDD") == 0xDD
    assert multi_info("1c") == 0x1C
    type_word = t._hex_value("key_extraction_type_word", 4)
    assert type_word("01 80") == 0x0180
    assert type_word(0x0400) == 0x0400
    expect_invalid(multi_info, 0x100, "between 0x00 and 0xFF")
    expect_invalid(multi_info, "0x1DC", "between 0x00 and 0xFF")
    expect_invalid(multi_info, "DX", "must be a hex value")
    expect_invalid(multi_info, True, "must be a hex value")
    expect_invalid(type_word, -1, "between 0x00 and 0xFFFF")


@case("hub_validators.validate_node_id")
def node_id_is_three_hex_bytes():
    from esphome.components.home_io_control import hub_validators as v

    assert v.validate_node_id("C0FFEE") == "C0FFEE"
    expect_invalid(v.validate_node_id, "C0FFE", "exactly 6 hex characters")
    expect_invalid(v.validate_node_id, "C0FFEG", "valid hexadecimal")


@case("hub_validators.validate_system_key")
def system_key_is_sixteen_hex_bytes():
    from esphome.components.home_io_control import hub_validators as v

    key = "0123456789ABCDEF0123456789ABCDEF"
    assert v.validate_system_key(key) == key
    expect_invalid(v.validate_system_key, key[:-2], "exactly 32 hex characters")
    expect_invalid(v.validate_system_key, key[:-1] + "Z", "valid hexadecimal")


@case("hub_validators.validate_device_id")
def device_id_is_three_hex_bytes():
    from esphome.components.home_io_control import hub_validators as v

    assert v.validate_device_id("FEEB1E") == "FEEB1E"
    expect_invalid(v.validate_device_id, "FEEB1", "exactly 6 hex characters")
    expect_invalid(v.validate_device_id, "FEEB1X", "valid hexadecimal")


@case()
def companion_joins_its_parents_sub_device_unless_it_names_its_own():
    """A companion groups under the parent entity's ESPHome sub-device; a nested user companion
    that sets its own `device_id:` keeps it; without a parent sub-device the key stays absent."""
    from esphome.const import CONF_DEVICE_ID
    from esphome.components.home_io_control import hub_validators as v

    parent = {CONF_DEVICE_ID: "dev_parent"}
    assert v.inherit_esphome_device({}, parent) == {CONF_DEVICE_ID: "dev_parent"}
    own = {CONF_DEVICE_ID: "dev_own"}
    assert v.inherit_esphome_device(own, parent) == {CONF_DEVICE_ID: "dev_own"}
    assert v.inherit_esphome_device({}, {}) == {}


@case("hub_validators.validate_device_type")
def device_type_takes_a_name_or_a_byte():
    from esphome.components.home_io_control import hub_validators as v

    assert v.validate_device_type("vertical_exterior_awning") == v.DEVICE_TYPE_OPTIONS["vertical_exterior_awning"]
    assert v.validate_device_type("electrical_heater") == 0x34
    assert v.validate_device_type("0x11") == 0x11
    assert v.validate_device_type(0x11) == 0x11
    # Node types are ten bits wide, so a remote controller (1023) is a legal raw value.
    assert v.validate_device_type("0x3FF") == 0x3FF
    # The two rejection sites: a string that is neither a name nor a number, and a value that is
    # neither a string nor an int. An out-of-range number is cv.int_range's own message.
    expect_invalid(v.validate_device_type, "awnig", "known name or an integer in the range 0..1023 (for example 0x11)")
    expect_invalid(v.validate_device_type, 1.5, "known name or an integer in the range 0..1023")
    expect_invalid(v.validate_device_type, 1024, "at most 1023")


@case("hub_validators._resolve_device_type_token")
def renamed_device_types_name_their_replacement():
    from esphome.components.home_io_control import hub_validators as v

    expect_invalid(v.validate_device_type, "awning", "use 'vertical_exterior_awning' instead")
    expect_invalid(v.validate_device_type, "blind", "use 'vertical_interior_blind' instead")
    expect_invalid(v.validate_device_type, "venetian_blind", "use 'interior_venetian_blind' instead")
    expect_invalid(v.validate_device_type, "dual_shutter", "use 'dual_roller_shutter' instead")
    expect_invalid(v.validate_device_type, "external_venetian_blind", "use 'exterior_venetian_blind' instead")
    expect_invalid(v.validate_device_type, "screen", "type 11 is a secure configuration device")
    expect_invalid(v.validate_device_type, "SCREEN", "'vertical_exterior_awning' or 'vertical_interior_blind'")
    # The class form of linked_remotes shares the resolver, so it gives the same migration message.
    expect_invalid(v.validate_linked_remote_entry, "class:awning", "use 'vertical_exterior_awning' instead")
    # Every renamed name has a replacement that is a current option (or the screen explanation).
    for old, message in v.RENAMED_DEVICE_TYPES.items():
        assert old not in v.DEVICE_TYPE_OPTIONS, old


@case("hub_validators.validate_manufacturer")
def manufacturer_takes_a_name_or_a_byte():
    from esphome.components.home_io_control import hub_validators as v

    assert v.validate_manufacturer("somfy") == v.MANUFACTURER_OPTIONS["somfy"]
    assert v.validate_manufacturer("0x02") == 0x02
    assert v.validate_manufacturer(0x02) == 0x02
    expect_invalid(v.validate_manufacturer, "somfi", "known name or an integer in the range 0..255 (for example 0x02)")
    expect_invalid(v.validate_manufacturer, 1.5, "known name or an integer in the range 0..255")
    expect_invalid(v.validate_manufacturer, 256, "at most 255")


@case("hub_validators.validate_radio_transport")
def spi_radio_needs_its_chip_select_pin():
    from esphome.components.home_io_control import hub_validators as v

    # Every radio type reaches its chip over SPI today; a new one has to be placed deliberately.
    assert set(v.RADIO_TYPE_OPTIONS) == set(v.SPI_RADIO_TYPES)
    with_cs = {"radio_type": "sx1262", "cs_pin": 8}
    assert v.validate_radio_transport(with_cs) is with_cs
    expect_invalid(v.validate_radio_transport, {"radio_type": "lr1121"}, "radio_type: lr1121 is an SPI radio")


def opt_in_key(reading_name):
    """The YAML key that declares a reading, as the mirror names it."""
    from esphome.components.home_io_control import hub_validators as v

    return v.DEVICE_READINGS[reading_name]["opt_in_key"]


@case("hub_validators._validate_min_interval")
def poll_intervals_have_a_floor():
    from esphome.components.home_io_control import hub_validators as v

    assert v.validate_status_poll_interval("60s") is not None
    expect_invalid(v.validate_status_poll_interval, "100ms", "status_poll_interval must be at least 500ms")
    assert v.validate_reading_poll_interval("RAIN")("15min") is not None
    assert v.validate_reading_poll_interval("RAIN")("1min") is not None
    expect_invalid(v.validate_reading_poll_interval("RAIN"), "59s", "rain_sensor_poll_interval must be at least 60000ms")
    assert v.validate_reading_poll_interval("SUN_LEVEL")("30min") is not None
    assert v.validate_reading_poll_interval("SUN_LEVEL")("1min") is not None
    expect_invalid(v.validate_reading_poll_interval("SUN_LEVEL"), "59s", "sun_level_poll_interval must be at least 60000ms")
    assert v.validate_reading_poll_interval("BATTERY_LEVEL")("6h") is not None
    assert v.validate_reading_poll_interval("BATTERY_LEVEL")("1min") is not None
    expect_invalid(
        v.validate_reading_poll_interval("BATTERY_LEVEL"), "59s", "battery_level_poll_interval must be at least 60000ms"
    )
    # The keys come from the mirror's rows with a floor, and every such row is a cover reading's opt-in key.
    keys = {str(k) for k in v.reading_poll_interval_schema("cover")}
    assert keys == {"rain_sensor_poll_interval", "sun_level_poll_interval", "battery_level_poll_interval"}, keys
    assert v.validate_status_refresh_interval("15min") is not None
    assert v.validate_status_refresh_interval("1min") is not None
    expect_invalid(v.validate_status_refresh_interval, "59s", "status_refresh_interval must be at least 60000ms")


@case("hub_validators.validate_linked_remote_entry")
def linked_remote_entry_takes_a_class_or_an_address():
    from esphome.components.home_io_control import hub_validators as v

    assert v.validate_linked_remote_entry("class:vertical_exterior_awning") is not None
    assert v.validate_linked_remote_entry("class:0x14") is not None
    assert v.validate_linked_remote_entry("A1B2C3") is not None
    expect_invalid(v.validate_linked_remote_entry, "class:awnig", "Unknown device class 'awnig'")


@case("lr1121_update_codegen.validate_checksum_md5")
def checksum_md5_is_thirty_two_hex_characters():
    from esphome.components.home_io_control import lr1121_update_codegen as l

    md5 = "0123456789abcdef0123456789abcdef"
    assert l.validate_checksum_md5(md5.upper()) == md5
    expect_invalid(l.validate_checksum_md5, md5[:-1], "exactly 32 hex characters")
    expect_invalid(l.validate_checksum_md5, md5[:-1] + "x", "valid hexadecimal")


@case("lr1121_update_codegen.validate_lr1121_firmware_source")
def firmware_source_checks_shape_and_image_class():
    from esphome.components.home_io_control import lr1121_update_codegen as l

    base = "github://Lora-net/radio_firmware_images/lr1121"
    transceiver = f"{base}/transceiver/lr1121_transceiver_0104.bin"
    loader = f"{base}/loader/lr1121_loader_2101.bin"
    assert l.validate_lr1121_firmware_source(transceiver) == transceiver
    assert l.validate_lr1121_firmware_source(loader, expect_loader=True) == loader
    expect_invalid(l.validate_lr1121_firmware_source, "https://example.com/fw.bin", "expected github://")
    expect_invalid(l.validate_lr1121_firmware_source, loader, "only usable through the bootloader: sub-block")
    expect_invalid(lambda v: l.validate_lr1121_firmware_source(v, expect_loader=True), transceiver,
                   "bootloader: source must be a bootloader loader image")


@case("oneway_controllers._no_duplicate_enrollment_classes")
def enrollment_classes_reject_repeats():
    from esphome.components.home_io_control import oneway_controllers as o
    from esphome.components.home_io_control.hub_validators import DEVICE_TYPE_OPTIONS

    # Runs after each entry was resolved to its device-type byte; the message names it again.
    awning, shutter = DEVICE_TYPE_OPTIONS["vertical_exterior_awning"], DEVICE_TYPE_OPTIONS["roller_shutter"]
    assert o._no_duplicate_enrollment_classes([awning, shutter]) == [awning, shutter]
    expect_invalid(o._no_duplicate_enrollment_classes, [awning, awning], "'vertical_exterior_awning' more than once")
    expect_invalid(o._no_duplicate_enrollment_classes, [0xFE, 0xFE], "'0xfe' more than once")


@case()
def derived_oneway_node_id_is_stable_and_per_identity():
    from esphome.components.home_io_control import oneway_controllers as o

    first = o.derive_oneway_node_id("C0FFEE", "awning_remote")
    assert first == o.derive_oneway_node_id("C0FFEE", "awning_remote"), "must not change between builds"
    assert len(first) == 6 and int(first, 16) >= 0
    assert first != o.derive_oneway_node_id("C0FFEE", "screen_remote"), "identities must differ"
    assert first != o.derive_oneway_node_id("C0FFEF", "awning_remote"), "hubs must differ"


@case()
def oneway_invert_position_is_tri_state_in_codegen_and_warns_for_inverted_broadcast_all():
    """`invert_position:` is emitted as an empty optional when absent (the kind decides in C++),
    as true/false when present; an inverted kind on `execute_broadcast: all` without the key logs
    one warning, and setting the key (or a non-inverted kind) logs none."""
    import logging

    from esphome.components.home_io_control import hub_names as n
    from esphome.components.home_io_control import oneway_controllers as o
    from esphome.components.home_io_control.hub_validators import DEVICE_TYPE_OPTIONS

    def identity(kind, broadcast, **extra):
        base = {
            "id": "remote",
            n.CONF_NODE_ID: "A11CE0",
            n.CONF_SYSTEM_KEY: "00112233445566778899AABBCCDDEEFF",
            n.CONF_MANUFACTURER: 0,
            n.CONF_IO_DEVICE_TYPE: DEVICE_TYPE_OPTIONS[kind],
            n.CONF_INITIAL_SEQUENCE: 0,
            n.CONF_ENROLLMENT_WITH_MAC: False,
            n.CONF_EXECUTE_BROADCAST: broadcast,
            n.CONF_ENROLLMENT: False,
            n.CONF_COMMANDS: ["open"],
        }
        base.update(extra)
        return base

    def expression(**extra):
        return str(o.oneway_controller_expression(identity("horizontal_awning", "typed", **extra), "C0FFEE"))

    assert ".invert_override = {}" in expression()
    assert ".invert_override = true" in expression(**{n.CONF_INVERT_POSITION: True})
    assert ".invert_override = false" in expression(**{n.CONF_INVERT_POSITION: False})

    class Capture(logging.Handler):
        def __init__(self):
            super().__init__()
            self.messages = []

        def emit(self, record):
            self.messages.append(record.getMessage())

    def warnings_for(entry):
        capture = Capture()
        o._LOGGER.addHandler(capture)
        try:
            o.validate_oneway_controllers(
                {
                    n.CONF_NODE_ID: "C0FFEE",
                    n.CONF_SYSTEM_KEY: "00112233445566778899AABBCCDDEEFF",
                    n.CONF_RADIO_TYPE: "sx1262",
                    n.CONF_ONEWAY_CONTROLLERS: [entry],
                }
            )
        finally:
            o._LOGGER.removeHandler(capture)
        return [m for m in capture.messages if "mirrored" in m]

    assert len(warnings_for(identity("horizontal_awning", "all"))) == 1
    assert warnings_for(identity("horizontal_awning", "all", **{n.CONF_INVERT_POSITION: False})) == []
    assert warnings_for(identity("horizontal_awning", "typed")) == []
    assert warnings_for(identity("vertical_exterior_awning", "all")) == []


def _resolved_oneway(kind="horizontal_awning", radio_type="sx1262", manufacturer_explicit=True, **extra):
    """A `_ResolvedIdentity` for the one-way warning-rule cases, with the schema defaults filled."""
    from esphome.components.home_io_control import hub_names as n
    from esphome.components.home_io_control import oneway_controllers as o
    from esphome.components.home_io_control.hub_validators import DEVICE_TYPE_OPTIONS

    identity = {
        "id": "remote",
        n.CONF_NODE_ID: "A11CE0",
        n.CONF_SYSTEM_KEY: "00112233445566778899AABBCCDDEEFF",
        n.CONF_MANUFACTURER: 0,
        n.CONF_IO_DEVICE_TYPE: DEVICE_TYPE_OPTIONS[kind],
        n.CONF_ENROLLMENT_WITH_MAC: False,
        n.CONF_EXECUTE_BROADCAST: "typed",
        n.CONF_ENROLLMENT: False,
        n.CONF_COMMANDS: ["open"],
    }
    identity.update(extra)
    return o._ResolvedIdentity(identity, radio_type, manufacturer_explicit)


@case()
def oneway_warning_for_an_unprofiled_manufacturer_needs_an_explicit_vendor_that_can_transmit():
    from esphome.components.home_io_control import hub_names as n
    from esphome.components.home_io_control import oneway_controllers as o
    from esphome.components.home_io_control.hub_validators import ONEWAY_WIRE_PROFILE_MANUFACTURERS

    assert o._warn_unprofiled_manufacturer(_resolved_oneway(**{n.CONF_MANUFACTURER: 0x7F})) == (
        "home_io_control: oneway_controllers 'remote' has manufacturer 0x7F, which has no "
        "1W wire profile -- using the Somfy-shaped defaults (ACEI 0x43). Set execute_acei: "
        "explicitly if that is wrong for your device."
    )
    assert o._warn_unprofiled_manufacturer(
        _resolved_oneway(**{n.CONF_MANUFACTURER: sorted(ONEWAY_WIRE_PROFILE_MANUFACTURERS)[0]})
    ) is None
    assert o._warn_unprofiled_manufacturer(
        _resolved_oneway(manufacturer_explicit=False, **{n.CONF_MANUFACTURER: 0x7F})
    ) is None
    assert o._warn_unprofiled_manufacturer(
        _resolved_oneway(**{n.CONF_MANUFACTURER: 0x7F, n.CONF_COMMANDS: []})
    ) is None


@case()
def oneway_warning_for_enrollment_with_mac_applies_to_sx1276_only():
    from esphome.components.home_io_control import hub_names as n
    from esphome.components.home_io_control import oneway_controllers as o

    enrolling = {n.CONF_ENROLLMENT: True, n.CONF_ENROLLMENT_WITH_MAC: True}
    assert o._warn_enrollment_with_mac_on_sx1276(_resolved_oneway(radio_type="sx1276", **enrolling)) == (
        "home_io_control: oneway_controllers 'remote' has enrollment_with_mac: true on "
        "radio_type: sx1276, where the MAC-bearing 0x30 has been measured to take about "
        "twice as long per burst. No VELUX capture carries this MAC either. Consider "
        "leaving it false unless your hardware specifically needs the MAC-bearing form."
    )
    assert o._warn_enrollment_with_mac_on_sx1276(_resolved_oneway(radio_type="sx1262", **enrolling)) is None
    assert o._warn_enrollment_with_mac_on_sx1276(
        _resolved_oneway(radio_type="sx1276", **{n.CONF_ENROLLMENT: True})
    ) is None


@case()
def oneway_warning_for_a_velux_interior_blind_names_the_default_sweep():
    from esphome.components.home_io_control import hub_names as n
    from esphome.components.home_io_control import oneway_controllers as o
    from esphome.components.home_io_control.hub_validators import MANUFACTURER_OPTIONS

    velux = {n.CONF_MANUFACTURER: MANUFACTURER_OPTIONS["velux"], n.CONF_ENROLLMENT: True}
    assert o._warn_velux_interior_blind_default_sweep(_resolved_oneway("vertical_interior_blind", **velux)) == (
        "home_io_control: oneway_controllers 'remote' is a VELUX vertical_interior_blind with "
        "enrollment: true, but "
        "enrollment_classes: is unset, so the enrollment 0x30 sweep uses the exterior-shading "
        "default roller_shutter/vertical_exterior_awning/dual_roller_shutter and ignores "
        "io_device_type. This device may enroll on other classes: set enrollment_classes: (a KLI "
        "312 interior blind uses [vertical_interior_blind, interior_venetian_blind]). To find "
        "yours, press Gear on the existing remote and use the classes named in its 'rx 1W "
        "remote ... (0x2E)' DEBUG log lines -- see 'Finding your enrollment classes' in the "
        "1W transmit docs."
    )
    assert "interior_venetian_blind" in o._warn_velux_interior_blind_default_sweep(
        _resolved_oneway("interior_venetian_blind", **velux)
    ).split("is a VELUX ")[1].split(" ")[0]
    assert o._warn_velux_interior_blind_default_sweep(_resolved_oneway("roller_shutter", **velux)) is None
    assert o._warn_velux_interior_blind_default_sweep(
        _resolved_oneway("vertical_interior_blind", **velux, **{n.CONF_ENROLLMENT_CLASSES: [1]})
    ) is None
    assert o._warn_velux_interior_blind_default_sweep(
        _resolved_oneway("vertical_interior_blind", **{n.CONF_MANUFACTURER: 0, n.CONF_ENROLLMENT: True})
    ) is None


@case()
def oneway_warning_for_an_inverted_kind_on_broadcast_all_unless_invert_position_is_set():
    from esphome.components.home_io_control import hub_names as n
    from esphome.components.home_io_control import oneway_controllers as o

    broadcast = {n.CONF_EXECUTE_BROADCAST: "all"}
    assert o._warn_inverted_kind_broadcast_all(_resolved_oneway(**broadcast)) == (
        "home_io_control: oneway_controllers 'remote' is an inverted device kind with "
        "execute_broadcast: all -- open/close are mirrored for every receiver of this "
        "broadcast; set invert_position explicitly if this identity also drives "
        "non-awning devices."
    )
    assert o._warn_inverted_kind_broadcast_all(_resolved_oneway(**broadcast, **{n.CONF_INVERT_POSITION: False})) is None
    assert o._warn_inverted_kind_broadcast_all(_resolved_oneway("vertical_exterior_awning", **broadcast)) is None
    assert o._warn_inverted_kind_broadcast_all(_resolved_oneway()) is None
    assert o._warn_inverted_kind_broadcast_all(_resolved_oneway(**broadcast, **{n.CONF_COMMANDS: []})) is None


@case()
def oneway_warning_for_enrollment_classes_applies_only_to_velux_enrollment():
    from esphome.components.home_io_control import hub_names as n
    from esphome.components.home_io_control import oneway_controllers as o
    from esphome.components.home_io_control.hub_validators import MANUFACTURER_OPTIONS

    velux = MANUFACTURER_OPTIONS["velux"]
    classes = {n.CONF_ENROLLMENT_CLASSES: [1]}
    assert o._warn_inert_enrollment_classes(_resolved_oneway(**classes, **{n.CONF_ENROLLMENT: True})) == (
        "home_io_control: oneway_controllers 'remote' sets enrollment_classes: but it only "
        "affects the VELUX enrollment gesture (needs manufacturer: velux AND "
        "enrollment: true) -- it is ignored here."
    )
    assert o._warn_inert_enrollment_classes(
        _resolved_oneway(**classes, **{n.CONF_MANUFACTURER: velux})
    ) is not None
    assert o._warn_inert_enrollment_classes(
        _resolved_oneway(**classes, **{n.CONF_MANUFACTURER: velux, n.CONF_ENROLLMENT: True})
    ) is None
    assert o._warn_inert_enrollment_classes(_resolved_oneway()) is None


@case()
def oneway_enrollment_without_an_explicit_manufacturer_has_a_problem_text():
    from esphome.components.home_io_control import hub_names as n
    from esphome.components.home_io_control import oneway_controllers as o

    assert o._enrollment_manufacturer_problem(
        _resolved_oneway(manufacturer_explicit=False, **{n.CONF_ENROLLMENT: True})
    ).startswith("oneway_controllers id 'remote' has enrollment: true but no manufacturer: set.")
    assert o._enrollment_manufacturer_problem(_resolved_oneway(**{n.CONF_ENROLLMENT: True})) is None
    assert o._enrollment_manufacturer_problem(_resolved_oneway(manufacturer_explicit=False)) is None


@case()
def oneway_identity_resolves_defaults_and_declares_entity_ids():
    from esphome.components.home_io_control import hub_names as n
    from esphome.components.home_io_control import oneway_controllers as o

    hub = {n.CONF_NODE_ID: "C0FFEE", n.CONF_SYSTEM_KEY: "00112233445566778899AABBCCDDEEFF", n.CONF_RADIO_TYPE: "sx1262"}
    identity = {
        "id": "remote", n.CONF_COMMANDS: ["open", "close"], n.CONF_ENROLLMENT: True,
    }
    resolved = o._resolve_identity_defaults(identity, hub)
    assert resolved.identity[n.CONF_NODE_ID] == o.derive_oneway_node_id("C0FFEE", "remote")
    assert resolved.identity[n.CONF_NODE_ID_DERIVED] is True
    assert resolved.identity[n.CONF_SYSTEM_KEY] == hub[n.CONF_SYSTEM_KEY]
    assert resolved.identity[n.CONF_MANUFACTURER] == 0 and not resolved.manufacturer_explicit
    assert resolved.radio_type == "sx1262"

    o._inject_identity_entity_ids(identity)
    assert sorted(identity[n.CONF_BUTTON_IDS]) == ["close", "open"]
    assert identity[n.CONF_BUTTON_IDS]["open"].id == "remote_open"
    assert identity[n.CONF_LAST_COMMAND_SENSOR_ID].id == "remote_last_1w_command"
    assert identity[n.CONF_ENROLL_BUTTON_ID].id == "remote_enroll"


@case()
def tuning_enum_assignments_cover_every_enum_select_with_its_option_names():
    """Each enum select offers exactly the option names its assignment maps to a C++ enumerator,
    so a selectable option always generates code."""
    from esphome.components.home_io_control import tuning as t

    keys = [key for key, _field, _options in t._ENUM_ASSIGNMENTS]
    assert len(keys) == len(set(keys)), "an enum key is assigned twice"
    for key, field, options in t._ENUM_ASSIGNMENTS:
        assert t._SELECT_OPTIONS[key] == list(options), f"{key}: select options differ from the assignment table"
        assert field == key, f"{key}: the C++ field is named after the YAML key"
    enum_selects = set(t._SELECT_OPTIONS) - set(t._BOOL_PARAMS) - {
        t.CONF_PAIRING_DISCOVERY_COMMANDS, t.CONF_PAIRING_DISCOVERY_DESTINATION, t.CONF_PAIRING_DISCOVERY_PAYLOAD,
    }
    assert enum_selects == set(keys), f"enum selects without an assignment: {sorted(enum_selects - set(keys))}"


@case()
def hub_entity_ids_are_declared_at_validation_time():
    """ESPHome 2026.x sizes its component vector from IDs known at the end of validation, so the
    hub-flag entities must get their IDs here, under names derived from the hub ID."""
    from esphome.const import CONF_ID
    from esphome.core import ID
    from esphome.components.home_io_control import hub_entities as e
    from esphome.components.home_io_control import hub_names as n

    injectors = [
        (e.inject_accept_foreign_pairing_switch_id, n.CONF_ACCEPT_FOREIGN_PAIRING,
         n.CONF_ACCEPT_FOREIGN_PAIRING_SWITCH_ID, "accept_foreign_pairing_switch"),
        (e.inject_recover_oneway_key_switch_id, n.CONF_RECOVER_ONEWAY_KEY,
         n.CONF_RECOVER_ONEWAY_KEY_SWITCH_ID, "recover_oneway_key_switch"),
        (e.inject_scan_paired_devices_button_id, n.CONF_SCAN_PAIRED_DEVICES_BUTTON,
         n.CONF_SCAN_PAIRED_DEVICES_BUTTON_ID, "scan_paired_devices_button"),
        (e.inject_discover_and_pair_button_id, n.CONF_DISCOVER_AND_PAIR_BUTTON,
         n.CONF_DISCOVER_AND_PAIR_BUTTON_ID, "discover_and_pair_button"),
        (e.inject_discover_and_pair_result_sensor_id, n.CONF_DISCOVER_AND_PAIR_BUTTON,
         n.CONF_DISCOVER_AND_PAIR_RESULT_SENSOR_ID, "pairing_result_sensor"),
        (e.inject_device_event_sensor_id, n.CONF_DIAGNOSTIC_PROBES,
         n.CONF_DEVICE_EVENT_SENSOR_ID, "device_event_sensor"),
    ]
    for inject, flag, id_key, suffix in injectors:
        explicit = inject({CONF_ID: ID("my_hub", is_declaration=True), flag: True})
        assert explicit[id_key].id == f"my_hub_{suffix}", explicit[id_key].id
        assert explicit[id_key].is_declaration
        # An auto-generated hub ID is still unresolved (None) at this point.
        auto = inject({CONF_ID: ID(None, is_declaration=True), flag: True})
        assert auto[id_key].id == f"home_io_control_{suffix}", auto[id_key].id
        off = inject({CONF_ID: ID("my_hub", is_declaration=True), flag: False})
        assert id_key not in off, f"{suffix} declared although its flag is off"


@case()
def heater_window_companions_are_declared_at_validation_time():
    """The window sensor and detection switch of a heater that has them get their IDs while the
    schema validates (ESPHome 2026.x sizes its component vector from the IDs known by then); a
    heater kind without them, or one declared without a type, gets none."""
    from esphome.const import CONF_ID, CONF_NAME
    from esphome.core import ID
    from esphome.components.home_io_control import climate as c
    from esphome.components.home_io_control import hub_validators as v

    def config(device_type=None, subtype=None):
        cfg = {CONF_ID: ID("living", is_declaration=True), CONF_NAME: "Living"}
        if device_type is not None:
            cfg["io_device_type"] = device_type
        if subtype is not None:
            cfg["io_subtype"] = subtype
        return cfg

    heater = v.DEVICE_TYPE_OPTIONS["electrical_heater"]
    with_window = c._inject_companion_ids(config(heater, 1))
    assert with_window["_window_sensor_id"].id == "living_window_sensor"
    assert with_window["_window_sensor_id"].is_declaration
    assert with_window["_window_detection_switch_id"].id == "living_window_detection_switch"
    # Each companion is declared as the class of the entity that shows its reading.
    assert str(with_window["_window_sensor_id"].type).endswith("IOHomeDeviceReadingBinarySensor")
    assert str(with_window["_window_detection_switch_id"].type).endswith("IOHomeHeaterSettingSwitch")
    towel = config(heater, 1)
    towel["io_model"] = "towel_dryer"
    for cfg in (
        config(heater, 0),
        config(heater),
        config(),
        config(v.DEVICE_TYPE_OPTIONS["roller_shutter"], 1),
        towel,
    ):
        out = c._inject_companion_ids(cfg)
        assert "_window_sensor_id" not in out, cfg
        assert "_window_detection_switch_id" not in out, cfg


@case("platform_common.validate_reading_keys")
def heating_level_is_accepted_only_where_the_heater_reports_a_level():
    """`heating_level:` passes on a plain heater and a towel dryer (the REPORTS_HEATING_LEVEL capability of their
    rows). On a radiator, a heater with a light and a pilot-wire interface, from which the hub reads no level, it is
    refused with a hint to remove it; on another device type and on an entry with no declared type it is
    refused with the kind to declare."""
    import esphome.config_validation as cv
    from esphome.components.home_io_control import hub_validators as v
    from esphome.components.home_io_control import platform_common as pc

    key = "heating_level"
    heater = v.DEVICE_TYPE_OPTIONS["electrical_heater"]
    plain = {"io_device_type": heater, "io_subtype": 0}
    pilot_wire = {**plain, "io_model": "pilot_wire_interface"}
    dryer = {"io_device_type": heater, "io_subtype": 1, "io_model": "towel_dryer"}
    radiator = {"io_device_type": heater, "io_subtype": 1}
    with_light = {**radiator, "io_model": "heater_with_light"}
    for kind in (plain, dryer, {"io_device_type": heater}):
        cfg = {**kind, key: {}}
        assert climate_reading_keys(cfg) is cfg, kind
    untouched = dict(radiator)
    assert climate_reading_keys(untouched) is untouched
    for kind in (radiator, with_light, pilot_wire):
        try:
            climate_reading_keys({**kind, key: {}})
        except cv.Invalid as err:
            assert "heating_level is not available on electrical_heater" in str(err), str(err)
            assert "the hub reads no heating level from this heater" in str(err) and "remove heating_level:" in str(err), str(err)
        else:
            raise AssertionError(f"accepted heating_level on {kind}")
    for kind in ({"io_device_type": v.DEVICE_TYPE_OPTIONS["heat_pump"]}, {"io_device_type": v.DEVICE_TYPE_OPTIONS["heating_temperature_interface"]}, {}):
        try:
            climate_reading_keys({**kind, key: {}})
        except cv.Invalid as err:
            assert "heating_level needs an electrical heater: set io_device_type: electrical_heater" in str(err), str(err)
        else:
            raise AssertionError(f"accepted heating_level on {kind}")


@case()
def reading_keys_are_accepted_only_on_a_kind_with_their_capability():
    """`central_heating_mode:` passes on a towel dryer (the CENTRAL_HEATING_MODE capability of its row);
    `occupancy_detection:` and `occupancy:` pass on a radiator and a heater with a light (OCCUPANCY); `energy:` passes
    on a radiator, a towel dryer and a heater with a light (ENERGY_METER); `remaining_time:`, `boost_duration:` and
    `drying_duration:` pass on a towel dryer only (RUN_TIMERS). Each is refused
    on every other heater kind, another device and an entry with no declared type, with a message naming the
    kind to declare; an entry without the keys is untouched."""
    import esphome.config_validation as cv
    from esphome.components.home_io_control import hub_validators as v
    from esphome.components.home_io_control import platform_common as pc

    heater = v.DEVICE_TYPE_OPTIONS["electrical_heater"]
    heat_pump = v.DEVICE_TYPE_OPTIONS["heat_pump"]
    radiator = {"io_device_type": heater, "io_subtype": 1}
    dryer = {**radiator, "io_model": "towel_dryer"}
    with_light = {**radiator, "io_model": "heater_with_light"}
    pilot_wire = {"io_device_type": heater, "io_subtype": 0, "io_model": "pilot_wire_interface"}
    plain = {"io_device_type": heater, "io_subtype": 0}
    pump = {"io_device_type": heat_pump}

    untouched = dict(radiator)
    assert climate_reading_keys(untouched) is untouched

    accepted = {
        "central_heating_mode": (dryer,),
        "occupancy_detection": (radiator, with_light),
        "occupancy": (radiator, with_light),
        "energy": (radiator, dryer, with_light),
        "remaining_time": (dryer,),
        "boost_duration": (dryer,),
        "drying_duration": (dryer,),
        "heating_level": (plain, dryer),
    }
    refused = {
        "central_heating_mode": (radiator, with_light, plain, pilot_wire, pump, {}),
        "occupancy_detection": (dryer, plain, pilot_wire, pump, {}),
        "occupancy": (dryer, plain, pilot_wire, pump, {}),
        "energy": (plain, pilot_wire, pump, {}),
        "remaining_time": (radiator, with_light, plain, pilot_wire, pump, {}),
        "boost_duration": (radiator, with_light, plain, pilot_wire, pump, {}),
        "drying_duration": (radiator, with_light, plain, pilot_wire, pump, {}),
        "heating_level": (radiator, with_light, pilot_wire, pump, {}),
    }
    hint = {
        "central_heating_mode": "io_model: towel_dryer",
        "occupancy_detection": "io_model: heater_with_light",
        "occupancy": "io_model: heater_with_light",
        "energy": "io_model: towel_dryer or io_model: heater_with_light",
        "remaining_time": "io_model: towel_dryer",
        "boost_duration": "io_model: towel_dryer",
        "drying_duration": "io_model: towel_dryer",
        # A radiator, a heater with a light and a pilot-wire interface are told to drop the key; the others what to
        # declare.
        "heating_level": "heating_level",
    }
    for key, kinds in accepted.items():
        for kind in kinds:
            cfg = {**kind, key: {}}
            assert climate_reading_keys(cfg) is cfg, (key, kind)
    for key, kinds in refused.items():
        for kind in kinds:
            cfg = {**kind, key: {}}
            try:
                climate_reading_keys(cfg)
            except cv.Invalid as err:
                assert hint[key] in str(err) and str(err).startswith(key), (key, kind, str(err))
            else:
                raise AssertionError(f"accepted {key} on {kind}")


@case("platform_common.validate_reading_keys")
def the_water_heaters_reading_keys_need_a_water_heater_and_are_not_a_heaters():
    """`energy:`, `heat_pump_running:` and `electric_backup_running:` pass on a domestic hot water heater and are
    refused on any other kind, with the kind to declare; the heater's `energy:` is a key of the climate platform only,
    so a water heater's entry is judged by the water heater's row, and the generic reading entities show them."""
    import esphome.config_validation as cv
    from esphome.components.home_io_control import hub_validators as v
    from esphome.components.home_io_control import platform_common as pc
    from esphome.components.home_io_control import water_heater as wh

    water_heater = {"io_device_type": v.DEVICE_TYPE_OPTIONS["domestic_hot_water"]}
    shutter = {"io_device_type": v.DEVICE_TYPE_OPTIONS["roller_shutter"]}
    for key in ("energy", "heat_pump_running", "electric_backup_running"):
        cfg = {**water_heater, key: {}}
        assert pc.validate_reading_keys(cfg, platform=pc.WATER_HEATER_PLATFORM) is cfg, key
        try:
            pc.validate_reading_keys({**shutter, key: {}}, platform=pc.WATER_HEATER_PLATFORM)
        except cv.Invalid as err:
            assert str(err).startswith(f"{key} needs a domestic hot water heater"), str(err)
        else:
            raise AssertionError(f"accepted {key} on a roller shutter")
    # The climate platform does not take the water heater's keys, and the water heater platform not the heater's.
    heater_only = {**water_heater, "occupancy": {}}
    assert pc.validate_reading_keys(heater_only, platform=pc.WATER_HEATER_PLATFORM) is heater_only
    assert pc.validate_reading_keys({**water_heater, "heat_pump_running": {}}, platform=pc.CLIMATE_PLATFORM)
    # The schema builds the generic reading entities from the mirror.
    entries = {str(key): validator for key, validator in wh.CONFIG_SCHEMA.validators[0].schema.items()}
    for key, entity_class in (
        ("energy", "IOHomeDeviceReadingSensor"),
        ("heat_pump_running", "IOHomeDeviceReadingBinarySensor"),
        ("electric_backup_running", "IOHomeDeviceReadingBinarySensor"),
        ("heating_source", "IOHomeDhwHeatingSourceSensor"),
        ("boost", "IOHomeDhwBoostSwitch"),
    ):
        checked = entries[key]({"name": f"Class of {key}"})
        assert str(checked["id"].type).split("::")[-1] == entity_class, (key, checked["id"].type)


@case()
def every_reading_with_a_key_builds_its_entity_and_names_what_to_declare():
    """The reading mirror drives the climate keys: each reading with a key has an entity kind, a capability the
    features list knows and a hint to give where the capability is missing, and the schema offers exactly those keys;
    a reading without a key has no entity kind to build."""
    from esphome.components.home_io_control import hub_validators as v
    from esphome.components.home_io_control import platform_common as pc

    keyed = {name: entry for name, entry in v.DEVICE_READINGS.items() if entry["key"] is not None}
    assert {entry["key"] for entry in keyed.values()} == {
        "central_heating_mode",
        "occupancy_detection",
        "occupancy",
        "energy",
        "remaining_time",
        "boost_duration",
        "drying_duration",
        "heating_level",
        "heat_pump_running",
        "electric_backup_running",
        "heating_energy",
        "hot_water_energy",
        "outdoor_temperature",
    }
    # A key belongs to one platform: the heater's `energy:` and the water heater's are two rows of two platforms.
    platform_keys = [(entry["platform"], entry["key"]) for entry in keyed.values()]
    assert len(platform_keys) == len(set(platform_keys)), platform_keys
    for name, entry in v.DEVICE_READINGS.items():
        assert entry["feature"] in v.FEATURES or entry["feature"] is None, name
        assert entry["platform"] in ("climate", "water_heater", "sensor", "cover"), name
        assert (entry["platform"] == "sensor") == (entry["source"] == "data_object"), name
        assert (entry["platform"] == "water_heater") == (entry["feature"] == "WATER_HEATER"), name
        # A cover's reading is declared by a key of the cover, not by a capability of its kind.
        assert (entry["platform"] == "cover") == ("opt_in_key" in entry), name
        if "opt_in_key" in entry:
            assert entry["feature"] is None and entry["key"] is None and "companion" in entry, name
        assert entry["source"] in (
            "register",
            "derived",
            "data_object",
            "status",
            "status_parameter",
            "event",
            "limitation",
            "status_battery",
        ), name
        # A data object, an event, a limitation and a status reply are read without a register of the poll, and
        # a cover's derived flag needs none.
        assert bool(entry["registers"]) == (
            entry["source"] in ("register", "derived") and entry["platform"] != "cover"
        ), name
        if entry["source"] == "data_object":
            assert entry["update_interval_ms"] >= v.MIN_DEVICE_READING_INTERVAL_MS, name
        if entry["key"] is not None:
            assert entry["entity"] in ("sensor", "binary_sensor", "switch", "text_sensor"), name
            assert entry["feature"] in v.READING_FEATURE_HINTS, name
            assert "companion" not in entry, f"{name} has a key and a companion"
        else:
            # A reading without a key builds an entity only as a companion (every device of the capability gets it).
            assert (entry["entity"] is not None) == ("companion" in entry), name
        if "companion" in entry:
            assert entry["entity"] in ("sensor", "binary_sensor", "switch", "text_sensor"), name
            assert entry["companion_id"] and (entry["feature"] in v.FEATURES or "opt_in_key" in entry), name
    for platform in ("climate", "water_heater", "cover", "sensor"):
        schema_keys = {str(key) for key in pc.reading_keys_schema(platform)}
        assert schema_keys == {entry["key"] for entry in keyed.values() if entry["platform"] == platform}, platform
    assert {str(key) for key in pc.reading_keys_schema(pc.WATER_HEATER_PLATFORM)} == {
        "energy",
        "heat_pump_running",
        "electric_backup_running",
    }
    # The data-object readings in production are the Atlantic heat pump's (issue #244).
    assert {str(key) for key in pc.reading_keys_schema(pc.SENSOR_PLATFORM)} == {
        "heating_energy",
        "hot_water_energy",
        "outdoor_temperature",
    }
    # The companions are the window sensor and the detection switch, named and IDed from the mirror.
    companions = {
        name: entry
        for name, entry in v.DEVICE_READINGS.items()
        if "companion" in entry and entry["platform"] == "climate"
    }
    assert {name: (entry["companion"], entry["companion_id"]) for name, entry in companions.items()} == {
        "WINDOW_OPEN": ("Window", "window_sensor"),
        "OPEN_WINDOW_DETECTION": ("Open Window Detection", "window_detection_switch"),
    }
    assert companions["OPEN_WINDOW_DETECTION"]["disabled_by_default"] is True
    assert companions["WINDOW_OPEN"]["device_class"] == "window"
    try:
        pc._reading_entity_schema({"key": "x", "entity": None})
    except ValueError:
        pass
    else:
        raise AssertionError("a reading with a key and no entity kind must not build a schema")


@case()
def the_sensor_platform_never_offers_a_heater_companion_or_status_reading():
    """`sensor:` offers the keys of data-object readings only, whatever the mirror holds: no heater register, derived or
    status reading (the heating level) is a key of it, and no companion (the window sensor, the detection switch) is.
    The data-object readings of the mirror (an Atlantic heat pump's) are exactly what it offers."""
    from esphome.components.home_io_control import hub_validators as v
    from esphome.components.home_io_control import platform_common as pc
    from esphome.components.home_io_control import sensor as sensor_platform

    offered = {str(key) for key in pc.reading_keys_schema(pc.SENSOR_PLATFORM)}
    heater_keys = {
        entry["key"]
        for entry in v.DEVICE_READINGS.values()
        if entry["key"] is not None and entry["source"] != "data_object"
    }
    data_object_keys = {entry["key"] for entry in v.DEVICE_READINGS.values() if entry["source"] == "data_object"}
    assert not offered & heater_keys, offered
    assert offered == data_object_keys, offered
    assert not pc._reading_companions(pc.SENSOR_PLATFORM)
    schema_keys = {str(key) for key in sensor_platform.CONFIG_SCHEMA.validators[0].schema}
    assert not schema_keys & heater_keys, schema_keys
    assert "heating_level" not in schema_keys


def with_data_object_reading(body):
    """Run `body(sensor_module)` with the energy reading mirrored as a data-object reading, the first kind the
    `sensor:` platform offers, on a heater kind whose capability the tests below exercise. The mirror is restored
    afterwards, and the platform schema is built after the change."""
    from esphome.components.home_io_control import hub_validators as v
    from esphome.components.home_io_control import sensor as sensor_platform

    original = v.DEVICE_READINGS["ENERGY_TOTAL"]
    v.DEVICE_READINGS["ENERGY_TOTAL"] = {
        "feature": "ENERGY_METER",
        "source": "data_object",
        "platform": "sensor",
        "registers": (),
        "key": "energy",
        "entity": "sensor",
        "unit": "kWh",
        "accuracy": 0,
        "device_class": "energy",
        "state_class": "total_increasing",
        "update_interval_ms": 3600000,
    }
    try:
        return body(sensor_platform)
    finally:
        v.DEVICE_READINGS["ENERGY_TOTAL"] = original


_SENSOR_ENTRIES = []


def sensor_entry(**extra):
    """The user-visible keys of a `sensor:` entry for a heater that counts its energy (a radiator). Every call gives
    the entry and its energy key a name of their own: ESPHome rejects a name used twice in one process."""
    _SENSOR_ENTRIES.append(None)
    number = len(_SENSOR_ENTRIES)
    if "energy" in extra:
        extra = {**extra, "energy": {"name": f"Energy {number}"}}
    return {
        "name": f"Counter {number}",
        "io_device_id": "ABC123",
        "io_device_type": "electrical_heater",
        "io_subtype": 1,
        **extra,
    }


@case("platform_common.validate_reading_keys")
def a_sensor_only_device_takes_the_numeric_readings_of_its_kind_from_the_mirror():
    """The `sensor:` platform declares a device with no other main entity: it offers the data-object readings'
    keys, takes an update_interval, hides its own entity and accepts a key only where the kind has the capability."""
    import esphome.config_validation as cv
    from esphome.const import CONF_INTERNAL

    def body(sensor_platform):
        schema = sensor_platform._config_schema()
        entry = {"energy": {"name": "Energy"}, "update_interval": "1h"}
        checked = schema(sensor_entry(**entry))
        assert checked[CONF_INTERNAL] is True, "the declaration entity shows nothing"
        assert checked["update_interval"].total_milliseconds == 3600000
        assert "energy" in checked
        # A device without the reading's capability is told what to declare.
        try:
            schema({**sensor_entry(**entry), "io_device_type": "roller_shutter", "io_subtype": 0})
        except cv.Invalid as err:
            assert "energy needs a heater that counts its energy" in str(err), str(err)
        else:
            raise AssertionError("a roller shutter has no energy counter")
        # A kind that is declared nowhere offers no key at all.
        undeclared = {key: value for key, value in sensor_entry(**entry).items() if not key.startswith("io_s")}
        del undeclared["io_device_type"]
        try:
            schema(undeclared)
        except cv.Invalid:
            pass
        else:
            raise AssertionError("an undeclared kind must not take a reading key")
        # A reading that is a data object is not offered to a climate entry, which reads heater registers.
        from esphome.components.home_io_control import platform_common as pc

        assert "energy" not in {str(key) for key in pc.reading_keys_schema(pc.CLIMATE_PLATFORM)}

    with_data_object_reading(body)


@case()
def the_water_heater_offers_its_companion_keys_with_their_home_assistant_metadata():
    """The water heater's opt-in entities keep their keys, units, accuracy, device and state classes: `energy:` in
    kWh with three decimals for the energy dashboard, the two running binary sensors, and the heating-source text
    sensor and boost switch with no metadata of their own."""
    from esphome.components.home_io_control import water_heater as wh

    entries = {str(key): validator for key, validator in wh.CONFIG_SCHEMA.validators[0].schema.items()}
    expected = {
        "energy": {
            "unit_of_measurement": "kWh",
            "accuracy_decimals": 3,
            "device_class": "energy",
            "state_class": "total_increasing",
        },
        "heat_pump_running": {"device_class": "running"},
        "electric_backup_running": {"device_class": "running"},
        "heating_source": {},
        "boost": {"restore_mode": "DISABLED"},
    }
    for key, metadata in expected.items():
        assert key in entries, f"the water_heater platform lost its {key} key"
        checked = entries[key]({"name": f"Water heater {key}"})
        for field, value in metadata.items():
            assert checked.get(field) == value, (key, field, checked.get(field))
    for key in ("energy", "heat_pump_running", "electric_backup_running", "heating_source", "boost"):
        assert key in {str(k) for k in entries}, key
    assert {"min_temperature", "max_temperature", "heating_level", "occupancy"}.isdisjoint(entries), (
        "no heater key belongs on a water heater"
    )


@case("hub_validators._validate_min_interval")
def a_data_object_reading_is_read_no_more_often_than_every_five_minutes():
    import esphome.config_validation as cv
    from esphome.components.home_io_control import hub_validators as v

    assert v.validate_device_reading_interval("5min").total_milliseconds == 300000
    expect_invalid(v.validate_device_reading_interval, "4min", "update_interval must be at least 300000ms")


@case()
def a_reading_key_asks_for_its_data_object_read_with_the_mirrors_default_interval():
    """The code the platform emits for a reading names the mirror's default interval; the entry's update_interval
    reaches the entity on its own, as ESPHome's set_update_interval()."""
    import asyncio

    import esphome.codegen as cg
    from esphome.components.home_io_control import platform_common as pc

    class Recorder:
        def __init__(self):
            self.calls = []

        def add_scheduled_reading(self, reading, interval_ms):
            self.calls.append(interval_ms)

    class Entity:
        def set_reading(self, reading):
            return None

    async def emit():
        recorder = Recorder()
        original_add, original_create = cg.add, pc.create_user_companion

        async def create_entity(*args, **kwargs):
            return Entity()

        cg.add = lambda expression: None
        pc.create_user_companion = create_entity
        try:
            await pc.reading_keys_to_code(recorder, None, {"energy": {}}, pc.SENSOR_PLATFORM)
        finally:
            cg.add, pc.create_user_companion = original_add, original_create
        return recorder.calls

    def body(_sensor_platform):
        assert asyncio.run(emit()) == [3600000], "the mirror's default"

    with_data_object_reading(body)


@case()
def cover_named_position_buttons_follow_the_declared_kind():
    """A cover gets one companion button per named position its declared kind has, with the entity
    ids and names Home Assistant already knows; a kind without the capability, a raw type with no row
    (including the ventilation point, which is not a cover) and a cover with no declared type get none."""
    from esphome.const import CONF_ID, CONF_NAME
    from esphome.core import ID
    from esphome.components.home_io_control import cover as c
    from esphome.components.home_io_control import hub_validators as v

    def config(device_type=None, subtype=None):
        cfg = {CONF_ID: ID("living", is_declaration=True), CONF_NAME: "Living"}
        if device_type is not None:
            cfg["io_device_type"] = device_type
        if subtype is not None:
            cfg["io_subtype"] = subtype
        return cfg

    def buttons(device_type=None, subtype=None):
        out = c._inject_companion_ids(config(device_type, subtype))
        return {key for key, (config_key, *_rest) in c.NAMED_POSITION_BUTTONS.items() if config_key in out}

    types = v.DEVICE_TYPE_OPTIONS
    assert buttons(types["roller_shutter"]) == {"FAVORITE"}
    assert buttons(types["window_opener"]) == {"FAVORITE", "VENT"}
    assert buttons(types["window_opener"], 58) == {"FAVORITE", "VENT"}, "named positions survive subtype 58"
    assert buttons(types["garage_opener"]) == {"PARTIAL"}, "a garage opener refuses the favourite (issue #163)"
    assert buttons(types["garage_opener"], 58) == {"PARTIAL"}, "5/58 inherits its family's partial position"
    assert buttons(types["gate_opener"]) == {"FAVORITE", "PEDESTRIAN"}
    assert buttons(types["gate_opener"], 58) == {"FAVORITE", "PEDESTRIAN"}, "7/58 inherits its family's buttons"
    assert buttons(types["swinging_shutter"], 58) == {"FAVORITE"}, "24/58 inherits its family's favourite"
    assert buttons(types["light"]) == set()
    assert buttons(types["unknown"]) == set()
    assert buttons() == set(), "a cover with no declared type gets none"
    assert buttons(0x14) == set(), "a raw ventilation point is not a cover and has no ventilation button"

    assert set(c.NAMED_POSITION_BUTTONS) == set(v.NAMED_POSITION_FEATURES)
    for key in c.NAMED_POSITION_BUTTONS:
        getattr(c.CoverCommand, key)  # every key names a CoverCommand value

    # Home Assistant entity ids and names depend on these.
    out = c._inject_companion_ids(config(types["window_opener"]))
    assert out[c.CONF_FAVORITE_BUTTON_ID].id == "living_favorite_button"
    assert out[c.CONF_VENT_BUTTON_ID].id == "living_vent_button"
    assert out[c.CONF_FAVORITE_BUTTON_ID].is_declaration
    assert c.NAMED_POSITION_BUTTONS["FAVORITE"][2] == "Favorite Position"
    assert c.NAMED_POSITION_BUTTONS["VENT"][2] == "Ventilation Position"

    # The experimental positions: pinned ids and names, disabled until the user enables them.
    out = c._inject_companion_ids(config(types["garage_opener"]))
    assert out[c.CONF_PARTIAL_BUTTON_ID].id == "living_partial_button"
    assert c.CONF_PEDESTRIAN_BUTTON_ID not in out and c.CONF_FAVORITE_BUTTON_ID not in out
    out = c._inject_companion_ids(config(types["gate_opener"]))
    assert out[c.CONF_PEDESTRIAN_BUTTON_ID].id == "living_pedestrian_button"
    assert c.CONF_PARTIAL_BUTTON_ID not in out
    assert c.NAMED_POSITION_BUTTONS["PARTIAL"][2] == "Partial Position"
    assert c.NAMED_POSITION_BUTTONS["PEDESTRIAN"][2] == "Pedestrian Position"
    for key in ("FAVORITE", "VENT"):
        assert c.NAMED_POSITION_BUTTONS[key][3] is False
    for key in ("PARTIAL", "PEDESTRIAN"):
        assert c.NAMED_POSITION_BUTTONS[key][3] is True, "experimental buttons start disabled"
    assert not buttons(types["roller_shutter"]) & {"PARTIAL", "PEDESTRIAN"}


@case()
def cover_travel_speed_declares_its_select_and_names_the_cpp_options():
    """`travel_speed:` adds the Travel Speed select and `silent:` the Silent Operation switch, never both;
    the select's options are the names travel_speed_name() publishes in C++, in enum order."""
    from esphome.const import CONF_ID, CONF_NAME
    from esphome.core import ID
    from esphome.components.home_io_control import cover as c

    def config(**extra):
        cfg = {CONF_ID: ID("living", is_declaration=True), CONF_NAME: "Living"}
        cfg.update(extra)
        return cfg

    out = c._inject_companion_ids(config(**{c.CONF_TRAVEL_SPEED: "default"}))
    # Home Assistant entity ids and names depend on these.
    assert out[c.CONF_TRAVEL_SPEED_SELECT_ID].id == "living_travel_speed_select"
    assert out[c.CONF_TRAVEL_SPEED_SELECT_ID].is_declaration
    assert c.CONF_SILENT_SWITCH_ID not in out
    assert c.travel_speed_select_name(out) == "Living Travel Speed"

    out = c._inject_companion_ids(config(**{c.CONF_SILENT: False}))
    assert out[c.CONF_SILENT_SWITCH_ID].id == "living_silent_switch"
    assert c.CONF_TRAVEL_SPEED_SELECT_ID not in out

    out = c._inject_companion_ids(config())
    assert c.CONF_TRAVEL_SPEED_SELECT_ID not in out and c.CONF_SILENT_SWITCH_ID not in out

    # tests/proto/proto_commands_test.cpp pins the same three names on the C++ side.
    assert list(c.TRAVEL_SPEEDS) == ["default", "silent", "fast"]
    assert [str(value) for value in c.TRAVEL_SPEEDS.values()] == [
        "home_io_control::TravelSpeed::MOTOR_DEFAULT",
        "home_io_control::TravelSpeed::SILENT",
        "home_io_control::TravelSpeed::FAST",
    ]


@case()
def cover_opt_in_companions_are_declared_only_with_their_key():
    """Each opt-in cover companion exists only when its key is set (`battery:` when true), under the id
    suffix Home Assistant entity ids are built from; the keys are the ones the reading mirror names."""
    from esphome.const import CONF_ID, CONF_NAME
    from esphome.core import ID
    from esphome.components.home_io_control import cover as c
    from esphome.components.home_io_control import hub_validators as v
    from esphome.components.home_io_control import platform_common as pc

    def declared(**extra):
        cfg = {CONF_ID: ID("living", is_declaration=True), CONF_NAME: "Living", **extra}
        return c._inject_companion_ids(cfg)

    # The mirror's opt-in keys are the cover's own keys.
    opt_in_keys = {entry["opt_in_key"] for entry in v.DEVICE_READINGS.values() if "opt_in_key" in entry}
    assert opt_in_keys == {
        opt_in_key("RAIN"),
        opt_in_key("SUN_LEVEL"),
        opt_in_key("BATTERY_LEVEL"),
        c.CONF_BATTERY,
    }
    opt_ins = {
        "_rain_sensor_id": ("living_rain_sensor", {opt_in_key("RAIN"): 1}),
        "_sun_level_sensor_id": ("living_sun_level_sensor", {opt_in_key("SUN_LEVEL"): 1}),
        "_battery_level_sensor_id": ("living_battery_level_sensor", {opt_in_key("BATTERY_LEVEL"): 1}),
        "_battery_low_sensor_id": ("living_battery_low_sensor", {c.CONF_BATTERY: True}),
        "_battery_sensor_id": ("living_battery_sensor", {c.CONF_BATTERY: True}),
    }
    # The id keys are the ones the mirror's companions are declared under.
    assert {pc._reading_companion_id_key(entry) for _, entry in pc._reading_opt_ins(pc.COVER_PLATFORM)} == set(opt_ins)
    bare = declared()
    for id_key, (expected_id, extra) in opt_ins.items():
        assert id_key not in bare
        out = declared(**extra)
        assert out[id_key].id == expected_id and out[id_key].is_declaration
    # `battery: false` is a key that is present but declares nothing.
    off = declared(**{c.CONF_BATTERY: False})
    assert "_battery_low_sensor_id" not in off and "_battery_sensor_id" not in off


@case()
def a_cover_schedules_the_readings_its_interval_keys_declare_and_asks_nothing_for_the_battery():
    """`reading_schedules_to_code()` gives the hub one scheduled reading per interval key of the cover, with the key's
    interval; `battery:` declares readings of the status reply, which are read by no schedule."""
    from unittest import mock

    import esphome.codegen as cg
    import esphome.config_validation as cv
    from esphome.components.home_io_control import platform_common as pc

    class Recorder:
        def __init__(self):
            self.calls = []

        def add_scheduled_reading(self, reading, interval_ms):
            self.calls.append((str(reading).split("::")[-1], interval_ms))

    def schedules(config):
        recorder = Recorder()
        with mock.patch.object(cg, "add", lambda *_: None):
            pc.reading_schedules_to_code(recorder, config, pc.COVER_PLATFORM)
        return recorder.calls

    minutes = cv.positive_time_period_milliseconds
    assert schedules({}) == []
    assert schedules({"battery": True}) == []
    assert schedules(
        {
            "rain_sensor_poll_interval": minutes("15min"),
            "sun_level_poll_interval": minutes("30min"),
            "battery_level_poll_interval": minutes("6h"),
            "battery": True,
        }
    ) == [("RAIN", 900000), ("SUN_LEVEL", 1800000), ("BATTERY_LEVEL", 21600000)]


@case()
def cover_opt_in_entities_are_created_with_their_names_ids_and_home_assistant_metadata():
    """The rain, sun level, battery level and battery entities of a cover keep their entity names, IDs, classes,
    units, categories and default visibility, whichever way the code builds them. The companions are captured where
    every one is bound to its device (platform_common._bind_companion), with the cover's own wiring mocked."""
    from unittest import mock

    import esphome.codegen as cg
    import esphome.config_validation as cv
    from esphome.components import cover as cover_platform
    from esphome.components.home_io_control import cover as c
    from esphome.components.home_io_control import platform_common as pc
    from esphome.const import CONF_ID, CONF_NAME
    from esphome.core import ID

    created = {}

    async def capture(config, parent, new_entity, entity_config, **new_kwargs):
        created[entity_config[CONF_ID].id] = (new_entity.__name__, entity_config)
        return mock.MagicMock()

    async def anything(*args, **kwargs):
        return mock.MagicMock()

    config = {
        CONF_ID: ID("living", is_declaration=True),
        CONF_NAME: "Living",
        pc.CONF_HOME_IO_CONTROL_ID: ID("hub"),
        pc.CONF_IO_DEVICE_ID: "ABC123",
        c.CONF_OPTIMISTIC_STATE: True,
        opt_in_key("RAIN"): cv.positive_time_period_milliseconds("15min"),
        opt_in_key("SUN_LEVEL"): cv.positive_time_period_milliseconds("30min"),
        opt_in_key("BATTERY_LEVEL"): cv.positive_time_period_milliseconds("6h"),
        c.CONF_BATTERY: True,
    }
    config = c._inject_companion_ids(config)
    with mock.patch.object(pc, "_bind_companion", capture), mock.patch.object(cg, "add", lambda *_: None), mock.patch.object(
        cg, "register_component", anything
    ), mock.patch.object(cg, "get_variable", anything), mock.patch.object(
        cover_platform, "new_cover", anything
    ), mock.patch.object(c, "wire_device_binding", anything), mock.patch.object(c, "create_companion_sensors", anything):
        asyncio.run(c.to_code(config))

    fields = (
        "name",
        "device_class",
        "unit_of_measurement",
        "accuracy_decimals",
        "state_class",
        "entity_category",
        "icon",
        "disabled_by_default",
    )
    expected = {
        "living_rain_sensor": ("new_binary_sensor", {"name": "Living Rain sensor", "device_class": "moisture"}),
        "living_sun_level_sensor": (
            "new_sensor",
            {
                "name": "Living Sun level",
                "device_class": "illuminance",
                "unit_of_measurement": "lx",
                "accuracy_decimals": 0,
                "state_class": "measurement",
                "icon": "mdi:white-balance-sunny",
            },
        ),
        "living_battery_level_sensor": (
            "new_sensor",
            {
                "name": "Living Battery level",
                "device_class": "battery",
                "unit_of_measurement": "%",
                "accuracy_decimals": 0,
                "state_class": "measurement",
            },
        ),
        "living_battery_low_sensor": (
            "new_binary_sensor",
            {"name": "Living Battery Low", "device_class": "battery", "entity_category": "diagnostic"},
        ),
        "living_battery_sensor": (
            "new_text_sensor",
            {
                "name": "Living Battery",
                "entity_category": "diagnostic",
                "icon": "mdi:battery",
                "disabled_by_default": True,
            },
        ),
    }
    assert set(created) >= set(expected), sorted(created)
    for entity_id, (creator, metadata) in expected.items():
        creator_name, entity_config = created[entity_id]
        assert creator_name == creator, (entity_id, creator_name)
        got = {field: entity_config[field] for field in fields if field in entity_config}
        got.pop("disabled_by_default", None) if not got.get("disabled_by_default") else None
        assert got == metadata, (entity_id, got, metadata)


@case()
def cover_gets_its_kinds_default_device_class():
    """A declared kind gets its Home Assistant class unless the YAML sets one (even ""); a cover with
    no declared type, or a kind with no default, gets none. The table is checked against the cover
    kinds, ESPHome's classes and the table on the cover docs page."""
    import re
    from esphome.const import CONF_DEVICE_CLASS
    from esphome.components.cover import DEVICE_CLASSES
    from esphome.components.home_io_control import cover as c
    from esphome.components.home_io_control import hub_validators as v

    types = v.DEVICE_TYPE_OPTIONS

    def classed(device_type=None, **extra):
        cfg = dict(extra)
        if device_type is not None:
            cfg["io_device_type"] = device_type
        return c._default_device_class(cfg).get(CONF_DEVICE_CLASS)

    assert classed(types["garage_opener"]) == "garage"
    assert classed(types["garage_opener"], device_class="door") == "door", "an explicit class wins"
    assert classed(types["garage_opener"], device_class="") == "", "an explicit empty class is kept"
    assert classed() is None, "no declared type, no class"
    assert classed(types["bioclimatic_pergola"]) is None
    assert classed(0x14) is None, "a raw type with no name has no default"

    for kind, device_class in v.DEFAULT_COVER_DEVICE_CLASS.items():
        assert kind in types, kind
        assert "LEVELS" in v.DEVICE_FEATURES.get(kind, frozenset()), f"{kind} is not a cover kind"
        assert device_class in DEVICE_CLASSES, device_class

    # The docs table is the same mapping, one row per class with the kinds that share it.
    page = (REPO / "docs" / "configuration" / "cover.md").read_text(encoding="utf-8")
    section = page.split("## Home Assistant device class", 1)[1].split("\n## ", 1)[0]
    documented = {}
    for kinds, device_class in re.findall(r"^\|\s*(`[^|]+`)\s*\|\s*`([a-z]+)`\s*\|$", section, re.M):
        for kind in re.findall(r"`([a-z_]+)`", kinds):
            documented[kind] = device_class
    assert documented == v.DEFAULT_COVER_DEVICE_CLASS, "docs/configuration/cover.md device class table"


@case()
def every_key_holding_a_secret_is_masked_in_the_config_dump():
    """Every system_key the component accepts (hub, device-bound entity, 1W controller) is marked
    cv.sensitive, so `esphome config` and the dashboard mask it. ESPHome's substring fallback that
    hid unmarked `*_key` fields goes away in 2026.12.0; an unmarked key would then print in clear."""
    import esphome.config_validation as cv
    from esphome.yaml_util import SensitiveStr

    from esphome.components.home_io_control import hub_names as n
    from esphome.components.home_io_control import oneway_controllers as o
    from esphome.components.home_io_control import platform_common as p

    def validator_for(schema, key):
        return next(v for k, v in schema.items() if str(k) == key)

    key = "0123456789ABCDEF0123456789ABCDEF"
    for label, schema in (
        ("device-bound entity", p.platform_schema_extension()),
        ("1W controller", o.ONEWAY_CONTROLLER_SCHEMA.schema),
    ):
        validator = validator_for(schema, n.CONF_SYSTEM_KEY)
        assert isinstance(validator, cv.SensitiveValidator), f"{label}: system_key is not cv.sensitive"
        assert isinstance(validator(key), SensitiveStr), label

    # The hub schema is a cv.All() chain; its first member holds the keys.
    import esphome.components.home_io_control as hub
    hub_schema = hub.CONFIG_SCHEMA.validators[0].schema
    assert isinstance(validator_for(hub_schema, n.CONF_SYSTEM_KEY), cv.SensitiveValidator), "hub"


@case("platform_common.require_feature")
def keys_a_kind_cannot_use_are_rejected_when_the_config_is_built():
    """`silent:` and `travel_speed:` need a kind that takes a travel-speed parameter; `dimmable: true`
    needs a light that is not on/off only. A config that declares no kind, or a kind with the feature,
    passes."""
    import esphome.config_validation as cv
    from esphome.components.home_io_control import cover as c
    from esphome.components.home_io_control import hub_validators as v
    from esphome.components.home_io_control import light as l

    t = v.DEVICE_TYPE_OPTIONS

    def silent(device_type=None, subtype=None, key=c.CONF_SILENT, value=True):
        cfg = {key: value}
        if device_type is not None:
            cfg["io_device_type"] = t[device_type]
        if subtype is not None:
            cfg["io_subtype"] = subtype
        return cfg

    def speed(device_type=None, subtype=None):
        return silent(device_type, subtype, key=c.CONF_TRAVEL_SPEED, value="default")

    assert c._require_travel_speed_kind(silent()) == silent(), "no declared kind: nothing says what the device takes"
    assert c._require_travel_speed_kind(speed()) == speed()
    c._require_travel_speed_kind(silent("roller_shutter"))
    c._require_travel_speed_kind(silent("exterior_venetian_blind"))
    c._require_travel_speed_kind(speed("window_opener"))
    c._require_travel_speed_kind({"io_device_type": t["garage_opener"]})  # neither key declared
    kinds = (
        ("interior_venetian_blind", None),
        ("dual_roller_shutter", None),
        ("garage_opener", None),
        ("garage_opener", 58),
        ("gate_opener", 58),
        ("swinging_shutter", 58),
    )
    for kind in kinds:
        try:
            c._require_travel_speed_kind(silent(*kind))
            raise AssertionError(f"silent was accepted on {kind}")
        except cv.Invalid as err:
            assert "remove silent:" in str(err) and "travel-speed" in str(err), str(err)
        try:
            c._require_travel_speed_kind(speed(*kind))
            raise AssertionError(f"travel_speed was accepted on {kind}")
        except cv.Invalid as err:
            assert "remove travel_speed:" in str(err) and "travel-speed" in str(err), str(err)

    def dimmable(device_type, subtype=None, value=True):
        cfg = {l.CONF_DIMMABLE: value, "io_device_type": t[device_type]}
        if subtype is not None:
            cfg["io_subtype"] = subtype
        return cfg

    l._require_dimmable_kind(dimmable("light"))  # 6/0: `dimmable:` decides, the Izymo dims
    l._require_dimmable_kind(dimmable("light", 1))
    l._require_dimmable_kind(dimmable("light", 58, value=False))
    try:
        l._require_dimmable_kind(dimmable("light", 58))
        raise AssertionError("dimmable was accepted on an on/off-only light")
    except cv.Invalid as err:
        assert "remove dimmable:" in str(err) and "on/off only" in str(err), str(err)


@case()
def device_features_fall_back_like_the_cpp_lookup():
    """The Python mirror answers the exact row, else the main-parameter-only variant at subtype 58,
    else the type's row; a node type YAML has no name for has no row."""
    from esphome.components.home_io_control import hub_validators as v

    t = v.DEVICE_TYPE_OPTIONS
    roller = v.device_features(t["roller_shutter"])
    assert v.device_features(t["roller_shutter"], 9) == roller, "an unnamed subtype takes the type row"
    assert v.device_features(t["roller_shutter"], 58) == {"MAIN_PARAMETER_ONLY", "FAVORITE"}
    assert "TILT" not in v.device_features(t["interior_venetian_blind"], 58)
    # VELUX KLF 200 API specification, Appendix 2: types 1 and 13 take the short position frame, type 10 has no slat.
    assert "TILT" in v.device_features(t["interior_venetian_blind"])
    for short_frame in ("interior_venetian_blind", "dual_roller_shutter"):
        assert "EXTENDED_POSITION_FRAME" not in v.device_features(t[short_frame]), short_frame
    assert "TILT" not in v.device_features(t["vertical_interior_blind"])
    assert "VENT" in v.device_features(t["window_opener"], 58)
    # Lights, switches and locks have no speed to choose, so they never take the speed block.
    for binary_kind in ("light", "on_off_switch", "lock"):
        assert v.device_features(t[binary_kind]) == frozenset(), binary_kind
        assert "EXTENDED_POSITION_FRAME" not in v.device_features(t[binary_kind], 1), binary_kind
    assert v.device_features(t["light"], 1) == {"LEVELS"}
    assert v.device_features(t["electrical_heater"], 1) == {"WINDOW_COMPANIONS", "OCCUPANCY", "ENERGY_METER"}
    assert v.device_features(t["electrical_heater"], 0) == {"REPORTS_HEATING_LEVEL"}
    assert v.device_features(0x14) == frozenset(), "a raw type with no name has no capabilities"
    assert v.device_features(0x14, 58) == {"MAIN_PARAMETER_ONLY"}


@case()
def the_mirror_names_only_real_capabilities_and_a_misspelt_name_is_refused():
    """Every capability the mirror lists is in FEATURES, the helpers refuse a name that is not, and the
    experimental status is never a capability."""
    from esphome.components.home_io_control import hub_validators as v
    from esphome.components.home_io_control import platform_common as p

    assert len(set(v.FEATURES)) == len(v.FEATURES)
    assert "EXPERIMENTAL" not in v.FEATURES
    for key, names in v.DEVICE_FEATURES.items():
        assert names <= set(v.FEATURES), f"{key} names {sorted(names - set(v.FEATURES))}"
    assert set(v.NAMED_POSITION_FEATURES.values()) <= set(v.FEATURES)
    for bad in (lambda: p.has_feature("LEVLES"), lambda: v.check_feature_name("experimental")):
        try:
            bad()
            raise AssertionError("a misspelt capability name was accepted")
        except ValueError:
            pass
    assert p.declared_has_feature({"io_device_type": v.DEVICE_TYPE_OPTIONS["roller_shutter"]}, "LEVELS")
    assert not p.declared_has_feature({}, "LEVELS"), "no declared kind has no capability"


@case()
def a_model_row_is_offered_only_by_its_kind_and_replaces_its_word():
    """A model (io_model:) is a row below one (io_device_type, io_subtype): the mirror lists it there
    only, its word replaces the kind's, a model the kind does not offer falls back to the kind, and
    the codegen reads the value from the entry."""
    from esphome.components.home_io_control import hub_validators as v
    from esphome.components.home_io_control import platform_common as p

    t = v.DEVICE_TYPE_OPTIONS
    towel = v.DEVICE_MODEL_OPTIONS["towel_dryer"]
    assert towel == 1
    assert v.device_models(t["electrical_heater"], 1) == ["heater_with_light", "towel_dryer"]
    assert v.device_models(t["electrical_heater"], 0) == ["pilot_wire_interface"]
    assert v.device_models(t["roller_shutter"]) == []
    assert v.device_models(0x14, 1) == [], "a raw type with no name has no models"
    assert "WINDOW_COMPANIONS" not in v.device_features(t["electrical_heater"], 1, towel)
    assert v.device_features(t["electrical_heater"], 1) == {"WINDOW_COMPANIONS", "OCCUPANCY", "ENERGY_METER"}, "no model: the radiator"
    assert v.device_features(t["roller_shutter"], 0, towel) == v.device_features(t["roller_shutter"])
    assert not v.kind_is_experimental(t["electrical_heater"], 1)
    assert p.config_model({"io_model": "towel_dryer"}) == towel
    assert p.config_model({}) == 0
    assert v.validate_device_model_name(" Towel_Dryer ") == "towel_dryer"
    entry = {"io_device_type": t["electrical_heater"], "io_subtype": 1, "io_model": "towel_dryer"}
    assert p.validate_device_model(entry) is entry, "a post-validator returns its config"
    assert p._kind_label(t["electrical_heater"], 1, "towel_dryer") == (
        "electrical_heater (io_subtype 1, io_model towel_dryer)"
    )


@case()
def experimental_kinds_are_marked_in_the_mirror_and_warned_about_once_at_config_time():
    """An experimental kind (no capture or report confirms it on a real device) is in
    EXPERIMENTAL_KINDS, apart from the capabilities; declaring one logs one warning that names the kind
    and the reporting page, and a confirmed kind, a declared kind of a confirmed type and a config with
    no kind log nothing."""
    import logging

    from esphome.components.home_io_control import hub_validators as v
    from esphome.components.home_io_control import platform_common as p

    t = v.DEVICE_TYPE_OPTIONS
    assert isinstance(v.EXPERIMENTAL_KINDS, frozenset)
    assert v.kind_is_experimental(t["gate_opener"], 58)
    assert v.kind_is_experimental(t["swinging_shutter"], 58)
    assert v.kind_is_experimental(t["light"], 1)
    assert v.kind_is_experimental(t["heat_pump"], 2), "a row with no capability is marked by the flag alone"
    assert not v.kind_is_experimental(t["garage_opener"], 58), "5/58 is confirmed by issue #163"
    assert not v.kind_is_experimental(t["gate_opener"])
    assert not v.kind_is_experimental(t["sliding_window"], 9), "a subtype with no row behaves like its type"
    assert not v.kind_is_experimental(0x14), "a raw type with no name"
    for unflagged in ("interior_venetian_blind", "vertical_interior_blind", "dual_roller_shutter"):
        assert not v.kind_is_experimental(t[unflagged]), f"{unflagged}: its behaviour is experimental, not its identity"

    class Capture(logging.Handler):
        def __init__(self):
            super().__init__()
            self.messages = []

        def emit(self, record):
            self.messages.append(record.getMessage())

    def warnings_for(config):
        capture = Capture()
        p._LOGGER.addHandler(capture)
        try:
            assert p.warn_if_experimental_kind(config) is config, "a post-validator returns its config"
        finally:
            p._LOGGER.removeHandler(capture)
        return capture.messages

    messages = warnings_for({"io_device_type": t["gate_opener"], "io_subtype": 58})
    assert len(messages) == 1, messages
    assert "gate_opener (io_subtype 58)" in messages[0]
    assert "not yet confirmed on a real device" in messages[0]
    assert "contributing.html#reporting-unsupported-devices" in messages[0]
    assert len(warnings_for({"io_device_type": t["light"], "io_subtype": 2})) == 1

    assert warnings_for({"io_device_type": t["garage_opener"], "io_subtype": 58}) == []
    assert warnings_for({"io_device_type": t["roller_shutter"]}) == []
    assert warnings_for({}) == [], "no declared kind: nothing says what the device is"


@case()
def lbt_max_retries_is_accepted_with_a_warning_and_generates_no_code():
    """`lbt_max_retries` is retired (ADR 0096): the tuning block still accepts it within its old range
    and logs one warning naming the replacement key; a value outside the range is still reported,
    and the key never reaches the generated tuning assignments."""
    import logging

    import esphome.config_validation as cv
    from esphome.components.home_io_control import tuning as t

    class Capture(logging.Handler):
        def __init__(self):
            super().__init__()
            self.messages = []

        def emit(self, record):
            self.messages.append(record.getMessage())

    capture = Capture()
    t._LOGGER.addHandler(capture)
    try:
        assert t.TUNING_SCHEMA({t.CONF_LBT_MAX_RETRIES: 3})[t.CONF_LBT_MAX_RETRIES] == 3
        try:
            t.TUNING_SCHEMA({t.CONF_LBT_MAX_RETRIES: 11})
            raise AssertionError("an out-of-range lbt_max_retries was accepted")
        except cv.Invalid:
            pass
    finally:
        t._LOGGER.removeHandler(capture)
    assert len(capture.messages) == 1, capture.messages
    assert "'lbt_max_retries:' is deprecated and has no effect" in capture.messages[0]
    assert "lbt_force_when_busy: true" in capture.messages[0]
    assert t.CONF_LBT_MAX_RETRIES not in t._NUMBER_PARAMS, "a deprecated key must not get a number entity"
    assert t.CONF_LBT_FORCE_WHEN_BUSY in t._BOOL_PARAMS, "the replacement key is a plain boolean tuning key"


@case()
def preamble_durations_take_fractional_milliseconds_inside_their_range():
    """Every preamble is a duration in ms (ADR 0097): the number entity and the YAML key share one
    range, a value inside it keeps its fraction, and one outside it is reported."""
    import esphome.config_validation as cv
    from esphome.components.home_io_control import tuning as t

    expected = {
        t.CONF_RESPONSE_PREAMBLE_MS: (1.5, 70, 0.5),
        t.CONF_START_PREAMBLE_MS: (1.5, 70, 0.5),
        t.CONF_COLD_BROADCAST_REPLY_PREAMBLE_MS: (1.5, 70, 0.5),
        t.CONF_PAIRING_DISCOVERY_PREAMBLE_MS: (1.5, 300, 0.5),
        t.CONF_LOW_POWER_WAKE_MS: (300, 1000, 10),
    }
    for key, (low, high, step) in expected.items():
        assert t._NUMBER_PARAMS[key] == (low, high, step, "ms"), (key, t._NUMBER_PARAMS[key])
    assert t.TUNING_SCHEMA({t.CONF_RESPONSE_PREAMBLE_MS: 3.5})[t.CONF_RESPONSE_PREAMBLE_MS] == 3.5
    assert t.TUNING_SCHEMA({t.CONF_LOW_POWER_WAKE_MS: 600})[t.CONF_LOW_POWER_WAKE_MS] == 600
    for key, value in ((t.CONF_RESPONSE_PREAMBLE_MS, 1.0), (t.CONF_START_PREAMBLE_MS, 71), (t.CONF_LOW_POWER_WAKE_MS, 299)):
        try:
            t.TUNING_SCHEMA({key: value})
        except cv.Invalid:
            continue
        raise AssertionError(f"{key}: {value} is outside its range and was accepted")
    for old_key in t._LEGACY_PREAMBLE_KEYS:
        assert old_key not in t._NUMBER_PARAMS, f"the deprecated {old_key} must not get a number entity"


@case()
def deprecated_preamble_byte_keys_warn_once_and_reach_codegen_unconverted():
    """Each old byte key is accepted within its old range, warns once naming its replacement, and
    is handed to C++ as bytes (setup() converts it for the radio); a response key of another radio
    is accepted and warned about but never emitted."""
    import logging

    import esphome.config_validation as cv
    from esphome.components.home_io_control import tuning as t

    class Capture(logging.Handler):
        def __init__(self):
            super().__init__()
            self.messages = []

        def emit(self, record):
            self.messages.append(record.getMessage())

    legacy = {
        "low_power_wake_preamble": (2048, "low_power_wake_ms"),
        "normal_start_preamble": (48, "start_preamble_ms"),
        "sx1262_response_preamble": (12, "response_preamble_ms"),
        "sx1276_response_preamble": (12, "response_preamble_ms"),
        "lr1121_response_preamble": (12, "response_preamble_ms"),
        "cold_broadcast_reply_preamble": (80, "cold_broadcast_reply_preamble_ms"),
        "pairing_discovery_preamble": (64, "pairing_discovery_preamble_ms"),
    }
    assert set(legacy) == set(t._LEGACY_PREAMBLE_KEYS), "every deprecated key is covered here"
    for old_key, (value, new_key) in legacy.items():
        capture = Capture()
        t._LOGGER.addHandler(capture)
        try:
            assert t.TUNING_SCHEMA({old_key: value})[old_key] == value
        finally:
            t._LOGGER.removeHandler(capture)
        assert len(capture.messages) == 1, (old_key, capture.messages)
        assert f"'{old_key}:' is deprecated" in capture.messages[0]
        assert f"'{new_key}:'" in capture.messages[0], capture.messages[0]
        low, high = t._LEGACY_PREAMBLE_KEYS[old_key][:2]
        try:
            t.TUNING_SCHEMA({old_key: high + 1})
        except cv.Invalid:
            pass
        else:
            raise AssertionError(f"{old_key}: {high + 1} is above its old byte range and was accepted")

    everything = {old_key: value for old_key, (value, _new) in legacy.items()}
    assert t._legacy_preamble_assignments(everything, "sx1276") == [
        ("legacy_preamble_bytes.low_power_wake", 2048),
        ("legacy_preamble_bytes.start", 48),
        ("legacy_preamble_bytes.response", 12),
        ("legacy_preamble_bytes.cold_broadcast_reply", 80),
        ("legacy_preamble_bytes.discovery", 64),
    ]
    assert t._legacy_preamble_assignments({"sx1262_response_preamble": 20}, "sx1276") == []
    assert t._legacy_preamble_assignments({"lr1121_response_preamble": 20}, "lr1121") == [
        ("legacy_preamble_bytes.response", 20)
    ]
    assert t._legacy_preamble_assignments({}, "sx1262") == []


@case("tuning._refuse_old_and_new_preamble_keys")
def a_byte_key_together_with_its_duration_key_is_refused():
    """Setting the old byte key and the duration key that replaces it is ambiguous and refused,
    naming the key to remove; the same keys for different preambles are fine."""
    import esphome.config_validation as cv
    from esphome.components.home_io_control import tuning as t

    for old_key, (_lo, _hi, new_key, _field, _radio) in t._LEGACY_PREAMBLE_KEYS.items():
        try:
            t._refuse_old_and_new_preamble_keys({old_key: 20, new_key: 10})
        except cv.Invalid as err:
            assert old_key in str(err) and new_key in str(err), str(err)
        else:
            raise AssertionError(f"{old_key} together with {new_key} was accepted")
    ok = {"normal_start_preamble": 48, t.CONF_LOW_POWER_WAKE_MS: 600}
    assert t._refuse_old_and_new_preamble_keys(ok) is ok


@case()
def silent_key_still_works_and_warns_once_with_the_exact_travel_speed_replacement():
    """`silent:` is deprecated: a cover that declares it is still valid and logs one warning that
    gives the travel_speed: value matching its boolean (false is fast, not default) and says the
    switch becomes a select. A cover without the key logs nothing."""
    import logging

    from esphome.components.home_io_control import cover as c
    from esphome.components.home_io_control import platform_common as p
    from esphome.core import ID

    class Capture(logging.Handler):
        def __init__(self):
            super().__init__()
            self.messages = []

        def emit(self, record):
            self.messages.append(record.getMessage())

    validator = p.deprecated_key(c.CONF_SILENT, c._silent_replacement)

    def warnings_for(config):
        capture = Capture()
        p._LOGGER.addHandler(capture)
        try:
            assert validator(config) is config, "a warn-only validator returns its config unchanged"
        finally:
            p._LOGGER.removeHandler(capture)
        return capture.messages

    def cover(**keys):
        return {"name": "Kitchen", "id": ID("kitchen", is_declaration=True), **keys}

    messages = warnings_for(cover(silent=True))
    assert len(messages) == 1, messages
    assert "'Kitchen' uses 'silent:'" in messages[0]
    assert "will become a config error" in messages[0]
    assert "'silent: true' with 'travel_speed: silent'" in messages[0]
    assert "'Kitchen Silent Operation' switch" in messages[0]
    assert "'Kitchen Travel Speed' select" in messages[0]

    messages = warnings_for(cover(silent=False))
    assert "'silent: false' with 'travel_speed: fast'" in messages[0], messages

    assert warnings_for(cover(travel_speed="silent")) == []
    assert warnings_for(cover()) == []


@case("platform_common.require_feature")
def feature_gated_keys_are_rejected_for_a_kind_without_the_feature():
    """require_feature() names the key, the kind and what to do; it passes when the config declares
    no kind, the key is inactive, or the kind has the feature."""
    import esphome.config_validation as cv
    from esphome.components.home_io_control import hub_validators as v
    from esphome.components.home_io_control import platform_common as p

    t = v.DEVICE_TYPE_OPTIONS
    silent_gate = dict(required="EXTENDED_POSITION_FRAME", reason="remove silent:")

    p.require_feature({}, "silent", True, **silent_gate)  # no declared kind: nothing says what it is
    p.require_feature({"io_device_type": t["garage_opener"]}, "silent", False, **silent_gate)
    p.require_feature({"io_device_type": t["roller_shutter"]}, "silent", True, **silent_gate)
    try:
        p.require_feature({"io_device_type": t["garage_opener"]}, "silent", True, **silent_gate)
        raise AssertionError("silent was accepted on a garage opener")
    except cv.Invalid as err:
        assert "silent" in str(err) and "garage_opener" in str(err) and "remove silent:" in str(err), str(err)
    try:
        p.require_feature({"io_device_type": t["roller_shutter"], "io_subtype": 58}, "silent", True, **silent_gate)
        raise AssertionError("silent was accepted on subtype 58")
    except cv.Invalid as err:
        assert "io_subtype 58" in str(err), str(err)

    on_off_only = dict(forbidden="MAIN_PARAMETER_ONLY", reason="remove dimmable:")
    p.require_feature({"io_device_type": t["light"]}, "dimmable", True, **on_off_only)
    try:
        p.require_feature({"io_device_type": t["light"], "io_subtype": 58}, "dimmable", True, **on_off_only)
        raise AssertionError("dimmable was accepted on an on/off-only light")
    except cv.Invalid as err:
        assert "dimmable" in str(err) and "remove dimmable:" in str(err), str(err)


def _raise_firmware_error(**_kwargs):
    from esphome.components.home_io_control import lr1121_firmware

    raise lr1121_firmware.Lr1121FirmwareError("checksum mismatch")


@case("lr1121_update_codegen.create_lr1121_firmware_update",
      "lr1121_update_codegen._create_lr1121_bootloader_update")
def firmware_fetch_errors_become_validation_errors():
    """A fetch/verify failure in to_code() is reported as a config error naming the block, not as
    a traceback. The fetch is replaced, so this needs no network."""
    import esphome.config_validation as cv
    from esphome.const import CONF_SOURCE
    from esphome.core import CORE
    from esphome.components.home_io_control import lr1121_firmware
    from esphome.components.home_io_control import lr1121_update_codegen as l
    from esphome.components.home_io_control import hub_names as n

    original_config_path = CORE.config_path
    CORE.config_path = Path("/tmp/hioc-py-test/fixture.yaml")
    original = lr1121_firmware.fetch_and_verify
    lr1121_firmware.fetch_and_verify = _raise_firmware_error
    try:
        config = {n.CONF_LR1121_FIRMWARE_UPDATE: {CONF_SOURCE: "github://o/r/transceiver.bin"}}
        try:
            asyncio.run(l.create_lr1121_firmware_update(config, var=None))
            raise AssertionError("fetch failure was not reported")
        except cv.Invalid as err:
            assert str(err).startswith("lr1121_firmware_update: checksum mismatch"), str(err)
        try:
            asyncio.run(l._create_lr1121_bootloader_update({CONF_SOURCE: "github://o/r/loader.bin"}, config,
                                                            var=None, cache_dir=Path("/tmp/hioc-py-test")))
            raise AssertionError("fetch failure was not reported")
        except cv.Invalid as err:
            assert str(err).startswith("lr1121_firmware_update.bootloader: checksum mismatch"), str(err)
    finally:
        lr1121_firmware.fetch_and_verify = original
        CORE.config_path = original_config_path


# =============================================================================================
# Fixture cases
# =============================================================================================


def load_fixtures():
    """[(path, expected_error_or_None, covers)] for every fixture YAML (files starting with `_`
    are shared includes, not fixtures)."""
    fixtures = []
    for path in sorted(FIXTURES.glob("*/*.yaml")):
        if path.name.startswith("_"):
            continue
        lines = path.read_text(encoding="utf-8").splitlines()
        first = lines[0] if lines else ""
        if first == "# expect: valid":
            expected = None
        elif first.startswith("# expect-error: "):
            expected = first[len("# expect-error: "):]
        else:
            raise SystemExit(f"{path}: first line must be '# expect: valid' or '# expect-error: <text>'")
        covers = ()
        if len(lines) > 1 and lines[1].startswith("# covers: "):
            covers = tuple(c.strip() for c in lines[1][len("# covers: "):].split(","))
        if (expected is None) != (path.parent.name == "valid"):
            raise SystemExit(f"{path}: valid fixtures live in valid/, rejections in invalid/")
        fixtures.append((path, expected, covers))
    return fixtures


def run_fixture(fixture):
    path, expected, _ = fixture
    result = subprocess.run(
        [sys.executable, "-m", "esphome", "config", str(path)],
        capture_output=True,
        text=True,
        cwd=path.parent,
        timeout=120,
        check=False,
    )
    output = result.stdout + result.stderr
    if expected is None:
        if result.returncode != 0 or "Configuration is valid!" not in output:
            return f"expected valid, got exit {result.returncode}:\n{output[-2000:]}"
        return None
    if result.returncode == 0:
        return f"expected rejection containing {expected!r}, but the config validated"
    # ESPHome indents the message under its config path; compare on whitespace-normalised text.
    # The failing config is echoed back too, so an expected text must be message wording, never
    # a value that also appears in the YAML.
    if " ".join(expected.split()) not in " ".join(output.split()):
        return f"rejected, but without {expected!r}:\n{output[-2000:]}"
    return None


# =============================================================================================
# Coverage gate
# =============================================================================================


def raising_functions():
    """{`<module>.<top-level function>`} for every top-level function that raises cv.Invalid
    (a raise inside a nested function counts for its enclosing top-level function)."""
    found = set()
    for path in sorted(COMPONENT_DIR.glob("*.py")):
        tree = ast.parse(path.read_text(encoding="utf-8"))
        for node in tree.body:
            if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                continue
            for sub in ast.walk(node):
                if isinstance(sub, ast.Raise) and sub.exc is not None:
                    target = sub.exc.func if isinstance(sub.exc, ast.Call) else sub.exc
                    if isinstance(target, ast.Attribute) and target.attr == "Invalid":
                        found.add(f"{path.stem}.{node.name}")
    return found


# =============================================================================================


def main():
    import_component()
    failures = []
    claimed = set()  # every path some case says it covers, whether that case passed or not

    for name, fn, covers in UNIT_CASES:
        claimed.update(covers)
        try:
            fn()
        except Exception:  # noqa: BLE001 -- report every failing case, not just the first
            failures.append(f"unit {name}:\n{traceback.format_exc()}")

    fixtures = load_fixtures()
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for fixture, problem in zip(fixtures, pool.map(run_fixture, fixtures)):
            claimed.update(fixture[2])
            if problem is not None:
                failures.append(f"fixture {fixture[0].relative_to(FIXTURES)}: {problem}")

    # A path claimed only by a failing case is reported through that case's failure above.
    needed = raising_functions()
    unknown = claimed - needed
    if unknown:
        failures.append(f"covers names no function raising cv.Invalid: {sorted(unknown)}")
    uncovered = needed - claimed
    if uncovered:
        failures.append(
            "no case covers these rejection paths (add a unit case or an invalid/ fixture "
            f"with '# covers: <module>.<function>'): {sorted(uncovered)}"
        )

    total = len(UNIT_CASES) + len(fixtures) + 1
    if failures:
        for failure in failures:
            print(f"FAIL {failure}\n", file=sys.stderr)
        print(f"run_tests.py: {len(failures)} FAILED of {total}", file=sys.stderr)
        return 1
    print(f"run_tests.py: OK ({len(UNIT_CASES)} unit, {len(fixtures)} fixture, "
          f"{len(needed)} rejection paths covered)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
