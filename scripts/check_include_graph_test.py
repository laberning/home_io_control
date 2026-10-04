#!/usr/bin/env python3
"""Self-tests for scripts/check-include-graph.py.

Stdlib `assert`-style, no pytest -- mirrors scripts/check_docs_links_test.py. Each case builds a
throwaway component directory in a tmpdir and runs the checker's ``check()`` against it.

Run via `python3 scripts/check_include_graph_test.py`; wired into `make include-graph`.
Exits non-zero if any case fails.
"""

from __future__ import annotations

import importlib.util
import sys
import tempfile
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent

_spec = importlib.util.spec_from_file_location("check_include_graph", SCRIPTS / "check-include-graph.py")
cig = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cig)

_failures: list[str] = []


def check(cond: bool, msg: str) -> None:
    if not cond:
        _failures.append(msg)


def run(files: "dict[str, list[str]]") -> "tuple[list[str], list[str], int]":
    """Build a component dir where each file includes the listed quoted headers, then check it."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for name, includes in files.items():
            body = "".join(f'#include "{inc}"\n' for inc in includes)
            (root / name).write_text(body, encoding="utf-8")
        return cig.check(root)


def rules(violations: "list[str]") -> "list[str]":
    """The `R<n> <file>` prefix of each violation, for order-independent comparison."""
    return sorted(" ".join(v.split()[:2]).rstrip(":") for v in violations)


# The minimal well-formed hub every case starts from.
BASE = {
    "hub_core.h": ["proto_frame.h"],
    "hub_internal.h": ["hub_core.h", "log_helpers.h"],
    "hub_status.cpp": ["hub_internal.h"],
    "log_helpers.h": ["proto_frame.h", "radio_interface.h"],
    "proto_frame.h": ["proto_sizes.h"],
    "proto_sizes.h": [],
    "radio_interface.h": ["proto_frame.h", "tuning_config.h"],
    "tuning_config.h": [],
    "platform_entity_base.h": ["hub_core.h"],
    "platform_cover.cpp": ["platform_entity_base.h", "entity_helpers.h"],
    "entity_helpers.h": ["log_helpers.h"],
}


def test_clean_tree_passes() -> None:
    violations, warnings, count = run(BASE)
    check(violations == [], f"clean tree: unexpected violations {violations}")
    check(count == len(BASE), f"clean tree: counted {count} files, expected {len(BASE)}")


def test_r1_collaborator_and_header_including_hub_internal() -> None:
    violations, _, _ = run({**BASE, "pairing_engine.cpp": ["hub_internal.h"], "helper.h": ["hub_internal.h"]})
    got = rules(violations)
    for want in ("R1 pairing_engine.cpp", "R1 helper.h"):
        check(want in got, f"R1: expected {want!r} in {got}")
    check("R1 hub_status.cpp" not in got, "R1: hub_*.cpp may include hub_internal.h")


def test_r2_transitive_reach_is_caught_with_its_chain() -> None:
    # A collaborator that includes an innocent-looking header which launders the hub.
    violations, _, _ = run({**BASE, "launder.h": ["hub_core.h"], "exchange_engine.cpp": ["launder.h"]})
    r2 = [v for v in violations if v.startswith("R2 exchange_engine.cpp")]
    check(len(r2) == 1, f"R2: expected one hit for exchange_engine.cpp, got {violations}")
    check(bool(r2) and "exchange_engine.cpp -> launder.h -> hub_core.h" in r2[0], f"R2: chain missing in {r2}")


def test_r2_allowlist_admits_listed_files() -> None:
    violations, _, _ = run({**BASE, "management_actions.cpp": ["hub_core.h"], "tuning_entities.cpp": ["hub_core.h"]})
    check(violations == [], f"R2 allowlist: unexpected violations {violations}")


def test_r2_stale_allowlist_entry_warns_without_failing() -> None:
    violations, warnings, _ = run({**BASE, "management_actions.cpp": ["log_helpers.h"]})
    check(violations == [], f"stale allowlist: must not fail, got {violations}")
    check(any("management_actions.cpp" in w for w in warnings), f"stale allowlist: no warning in {warnings}")


def test_r3_protocol_file_including_another_layer() -> None:
    violations, _, _ = run({**BASE, "proto_codecs.h": ["proto_frame.h", "radio_interface.h"]})
    check(rules(violations) == ["R3 proto_codecs.h"], f"R3: got {violations}")


def test_r4_radio_file_including_the_hub() -> None:
    violations, _, _ = run({**BASE, "radio_sx1262.cpp": ["radio_interface.h", "log_helpers.h"]})
    check("R4 radio_sx1262.cpp" in rules(violations), f"R4: got {violations}")


def test_r4_radio_extras_are_allowed() -> None:
    violations, _, _ = run({**BASE, "log_frame.h": [], "redaction.h": [],
                            "radio_sx1276.cpp": ["radio_interface.h", "log_frame.h", "redaction.h", "tuning_config.h"]})
    check(violations == [], f"R4 extras: unexpected violations {violations}")


def test_external_and_missing_includes_are_ignored() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "proto_frame.h").write_text(
            '#include "esphome/core/log.h"\n#include <string>\n#include "generated_image.h"\n', encoding="utf-8")
        violations, _, _ = cig.check(root)
    check(violations == [], f"external includes: unexpected violations {violations}")


def test_include_cycle_terminates() -> None:
    violations, _, _ = run({**BASE, "a.h": ["b.h"], "b.h": ["a.h", "hub_core.h"]})
    got = rules(violations)
    check("R2 a.h" in got and "R2 b.h" in got, f"cycle: expected R2 for both headers, got {got}")


def main() -> int:
    tests = [obj for name, obj in sorted(globals().items()) if name.startswith("test_") and callable(obj)]
    for test in tests:
        test()
    if _failures:
        for failure in _failures:
            print(f"FAIL: {failure}")
        return 1
    print(f"check_include_graph_test: OK ({len(tests)} tests)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
