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


@case("hub_validators._validate_min_interval")
def poll_intervals_have_a_floor():
    from esphome.components.home_io_control import hub_validators as v

    assert v.validate_status_poll_interval("60s") is not None
    expect_invalid(v.validate_status_poll_interval, "100ms", "status_poll_interval must be at least 500ms")
    assert v.validate_rain_sensor_poll_interval("15min") is not None
    assert v.validate_rain_sensor_poll_interval("1min") is not None
    expect_invalid(v.validate_rain_sensor_poll_interval, "59s", "rain_sensor_poll_interval must be at least 60000ms")


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
    assert with_window[c.CONF_WINDOW_SENSOR_ID].id == "living_window_sensor"
    assert with_window[c.CONF_WINDOW_SENSOR_ID].is_declaration
    assert with_window[c.CONF_WINDOW_DETECTION_SWITCH_ID].id == "living_window_detection_switch"
    for cfg in (config(heater, 0), config(heater), config(), config(v.DEVICE_TYPE_OPTIONS["roller_shutter"], 1)):
        out = c._inject_companion_ids(cfg)
        assert c.CONF_WINDOW_SENSOR_ID not in out, cfg
        assert c.CONF_WINDOW_DETECTION_SWITCH_ID not in out, cfg


@case()
def cover_named_position_buttons_follow_the_declared_kind():
    """A cover gets one companion button per named position its declared kind has, with the entity
    ids and names Home Assistant already knows; a kind without the bit, a raw type with no row
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
    assert buttons(types["garage_opener"]) == set(), "a garage opener refuses the favourite (issue #163)"
    assert buttons(types["garage_opener"], 58) == set()
    assert buttons(types["gate_opener"], 58) == {"FAVORITE"}, "7/58 inherits its family's favourite"
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


@case("platform_common.require_feature")
def keys_a_kind_cannot_use_are_rejected_when_the_config_is_built():
    """`silent:` needs a kind that takes a travel-speed parameter; `dimmable: true` needs a light that
    is not on/off only. A config that declares no kind, or a kind with the feature, passes."""
    import esphome.config_validation as cv
    from esphome.components.home_io_control import cover as c
    from esphome.components.home_io_control import hub_validators as v
    from esphome.components.home_io_control import light as l

    t = v.DEVICE_TYPE_OPTIONS

    def silent(device_type=None, subtype=None):
        cfg = {c.CONF_SILENT: True}
        if device_type is not None:
            cfg["io_device_type"] = t[device_type]
        if subtype is not None:
            cfg["io_subtype"] = subtype
        return cfg

    assert c._require_silent_kind(silent()) == silent(), "no declared kind: nothing says what the device takes"
    c._require_silent_kind(silent("roller_shutter"))
    c._require_silent_kind(silent("exterior_venetian_blind"))
    c._require_silent_kind({"io_device_type": t["garage_opener"]})  # `silent:` not declared at all
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
            c._require_silent_kind(silent(*kind))
            raise AssertionError(f"silent was accepted on {kind}")
        except cv.Invalid as err:
            assert "remove silent:" in str(err) and "travel-speed" in str(err), str(err)

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
    assert v.device_features(t["roller_shutter"], 58) == v.FEATURE_MAIN_PARAMETER_ONLY | v.FEATURE_FAVORITE
    assert v.device_features(t["interior_venetian_blind"], 58) & v.FEATURE_TILT == 0
    # VELUX KLF 200 API specification, Appendix 2: types 1 and 13 take the short position frame, type 10 has no slat.
    assert v.device_features(t["interior_venetian_blind"]) & v.FEATURE_TILT
    for short_frame in ("interior_venetian_blind", "dual_roller_shutter"):
        assert v.device_features(t[short_frame]) & v.FEATURE_EXTENDED_POSITION_FRAME == 0, short_frame
    assert v.device_features(t["vertical_interior_blind"]) & v.FEATURE_TILT == 0
    assert v.device_features(t["window_opener"], 58) & v.FEATURE_VENT
    assert v.device_features(t["lock"], 2) == 0, "an explicit subtype row beats the type row, even at zero"
    assert v.device_features(t["lock"], 1) == v.FEATURE_EXTENDED_POSITION_FRAME
    assert v.device_features(t["electrical_heater"], 1) == v.FEATURE_WINDOW_COMPANIONS
    assert v.device_features(t["electrical_heater"], 0) == 0
    assert v.device_features(0x14) == 0, "a raw type with no name has no capabilities"
    assert v.device_features(0x14, 58) == v.FEATURE_MAIN_PARAMETER_ONLY


@case("platform_common.require_feature")
def feature_gated_keys_are_rejected_for_a_kind_without_the_feature():
    """require_feature() names the key, the kind and what to do; it passes when the config declares
    no kind, the key is inactive, or the kind has the feature."""
    import esphome.config_validation as cv
    from esphome.components.home_io_control import hub_validators as v
    from esphome.components.home_io_control import platform_common as p

    t = v.DEVICE_TYPE_OPTIONS
    silent_gate = dict(required=v.FEATURE_EXTENDED_POSITION_FRAME, reason="remove silent:")

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

    on_off_only = dict(forbidden=v.FEATURE_MAIN_PARAMETER_ONLY, reason="remove dimmable:")
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
