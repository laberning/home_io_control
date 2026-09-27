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

    assert v.validate_device_type("awning") == v.DEVICE_TYPE_OPTIONS["awning"]
    assert v.validate_device_type("0x11") == 0x11
    assert v.validate_device_type(0x11) == 0x11
    # The two rejection sites: a string that is neither a name nor a number, and a value that is
    # neither a string nor an int. An out-of-range number is cv.int_range's own message.
    expect_invalid(v.validate_device_type, "awnig", "known name or an integer in the range 0..255 (for example 0x11)")
    expect_invalid(v.validate_device_type, 1.5, "known name or an integer in the range 0..255")
    expect_invalid(v.validate_device_type, 256, "at most 255")


@case("hub_validators.validate_manufacturer")
def manufacturer_takes_a_name_or_a_byte():
    from esphome.components.home_io_control import hub_validators as v

    assert v.validate_manufacturer("somfy") == v.MANUFACTURER_OPTIONS["somfy"]
    assert v.validate_manufacturer("0x02") == 0x02
    assert v.validate_manufacturer(0x02) == 0x02
    expect_invalid(v.validate_manufacturer, "somfi", "known name or an integer in the range 0..255 (for example 0x02)")
    expect_invalid(v.validate_manufacturer, 1.5, "known name or an integer in the range 0..255")
    expect_invalid(v.validate_manufacturer, 256, "at most 255")


@case("hub_validators.validate_status_poll_interval")
def status_poll_interval_has_a_floor():
    from esphome.components.home_io_control import hub_validators as v

    assert v.validate_status_poll_interval("60s") is not None
    expect_invalid(v.validate_status_poll_interval, "100ms", "at least 500ms")


@case("hub_validators.validate_linked_remote_entry")
def linked_remote_entry_takes_a_class_or_an_address():
    from esphome.components.home_io_control import hub_validators as v

    assert v.validate_linked_remote_entry("class:awning") is not None
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
    awning, shutter = DEVICE_TYPE_OPTIONS["awning"], DEVICE_TYPE_OPTIONS["roller_shutter"]
    assert o._no_duplicate_enrollment_classes([awning, shutter]) == [awning, shutter]
    expect_invalid(o._no_duplicate_enrollment_classes, [awning, awning], "'awning' more than once")
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
        (e._inject_accept_foreign_pairing_switch_id, n.CONF_ACCEPT_FOREIGN_PAIRING,
         n.CONF_ACCEPT_FOREIGN_PAIRING_SWITCH_ID, "accept_foreign_pairing_switch"),
        (e._inject_recover_oneway_key_switch_id, n.CONF_RECOVER_ONEWAY_KEY,
         n.CONF_RECOVER_ONEWAY_KEY_SWITCH_ID, "recover_oneway_key_switch"),
        (e._inject_scan_paired_devices_button_id, n.CONF_SCAN_PAIRED_DEVICES_BUTTON,
         n.CONF_SCAN_PAIRED_DEVICES_BUTTON_ID, "scan_paired_devices_button"),
        (e._inject_discover_and_pair_button_id, n.CONF_DISCOVER_AND_PAIR_BUTTON,
         n.CONF_DISCOVER_AND_PAIR_BUTTON_ID, "discover_and_pair_button"),
        (e._inject_discover_and_pair_result_sensor_id, n.CONF_DISCOVER_AND_PAIR_BUTTON,
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


def _raise_firmware_error(**_kwargs):
    from esphome.components.home_io_control import lr1121_firmware

    raise lr1121_firmware.Lr1121FirmwareError("checksum mismatch")


@case("lr1121_update_codegen._create_lr1121_firmware_update",
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

    CORE.config_path = Path("/tmp/hioc-py-test/fixture.yaml")
    original = lr1121_firmware.fetch_and_verify
    lr1121_firmware.fetch_and_verify = _raise_firmware_error
    try:
        config = {n.CONF_LR1121_FIRMWARE_UPDATE: {CONF_SOURCE: "github://o/r/transceiver.bin"}}
        try:
            asyncio.run(l._create_lr1121_firmware_update(config, var=None))
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
