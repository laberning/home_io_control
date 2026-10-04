#!/usr/bin/env python3
"""Include-graph layering check for components/home_io_control/.

The component's layers (docs/architecture_overview.md) only stay independent if their include
direction does: a collaborator that includes the hub's private helper header drags the whole hub
into its translation unit, and a single header that starts including it launders the hub into
every file that includes *that* header. Nothing in the compiler notices either, so this script
does.

It parses every quoted ``#include "..."`` in ``components/home_io_control/*.{h,cpp}``, keeps only
targets that exist in that directory, and computes each file's transitive include closure. Quoted
``esphome/...`` includes and all angle-bracket includes are ignored. Tests are exempt: they may
include anything.

Rules:

- R1: only the hub's own implementation files (``hub_*.cpp``) include ``hub_internal.h``. No
  header includes it.
- R2: only the files in ``HUB_CORE_ALLOWLIST`` reach ``hub_core.h``, directly or transitively.
  Every entry carries its reason. An entry that no longer reaches ``hub_core.h`` prints a warning,
  so the list can shrink as the hub's dependants narrow.
- R3: protocol files (``proto_*``) include only protocol headers.
- R4: radio files (``radio_*``) include only protocol and radio headers, plus the chip-neutral
  support headers in ``RADIO_EXTRA_HEADERS``.

Run via ``make include-graph`` (which also runs scripts/check_include_graph_test.py); it is part
of the ``lint`` composite target.
"""

import fnmatch
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
COMPONENT_DIR = REPO_ROOT / "components" / "home_io_control"

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.MULTILINE)

HUB_INTERNAL_H = "hub_internal.h"
HUB_CORE_H = "hub_core.h"

# (glob, reason) pairs: the files allowed to reach hub_core.h.
HUB_CORE_ALLOWLIST = [
    ("hub_core.h", "the hub itself"),
    ("hub_internal.h", "the hub's private helpers"),
    ("hub_*.cpp", "the hub's own implementation files"),
    ("platform_*.h", "entities bind to the hub through platform_entity_base.h"),
    ("platform_*.cpp", "entities bind to the hub through platform_entity_base.h"),
    ("tuning_entities.cpp", "the tuning entities call the hub's tuning API"),
    ("management_actions.cpp", "drives hub-level operations through IOHomeControlComponent *"),
]

# Chip-neutral support headers a radio driver may include besides proto_* and radio_*.
RADIO_EXTRA_HEADERS = {"tuning_config.h", "log_frame.h", "redaction.h"}


def _direct_includes(path: Path, known: set) -> "list[str]":
    """Project headers (by basename) that ``path`` includes directly."""
    text = path.read_text(encoding="utf-8")
    return [name for name in INCLUDE_RE.findall(text) if name in known]


def _closure_with_chains(start: str, graph: dict) -> dict:
    """Map every file ``start`` reaches transitively to one include chain that reaches it."""
    chains = {}
    stack = [(start, [start])]
    while stack:
        node, chain = stack.pop()
        for child in graph.get(node, []):
            if child in chains or child == start:
                continue
            chains[child] = chain + [child]
            stack.append((child, chain + [child]))
    return chains


def _allowlist_reason(name: str) -> "str | None":
    for pattern, reason in HUB_CORE_ALLOWLIST:
        if fnmatch.fnmatch(name, pattern):
            return reason
    return None


def check(component_dir: Path) -> "tuple[list[str], list[str], int]":
    """Apply R1-R4 to every ``.h``/``.cpp`` in ``component_dir``.

    Returns ``(violations, warnings, file_count)``.
    """
    files = sorted(p for p in component_dir.iterdir() if p.suffix in (".h", ".cpp"))
    known = {p.name for p in files if p.suffix == ".h"}
    graph = {p.name: _direct_includes(p, known) for p in files}

    violations = []
    warnings = []
    for name in sorted(graph):
        direct = graph[name]
        chains = _closure_with_chains(name, graph)

        if HUB_INTERNAL_H in direct and not (name.startswith("hub_") and name.endswith(".cpp")):
            violations.append(f"R1 {name}: includes {HUB_INTERNAL_H}, which only hub_*.cpp may include")

        reason = _allowlist_reason(name)
        if HUB_CORE_H in chains and reason is None:
            via = " -> ".join(chains[HUB_CORE_H])
            violations.append(f"R2 {name}: reaches {HUB_CORE_H} and is not allowlisted (via {via})")
        elif reason is not None and name != HUB_CORE_H and HUB_CORE_H not in chains:
            warnings.append(f"warning: {name} is allowlisted for {HUB_CORE_H} ({reason}) but no longer reaches it")

        if name.startswith("proto_"):
            for inc in direct:
                if not inc.startswith("proto_"):
                    violations.append(f"R3 {name}: protocol file includes non-protocol header {inc}")

        if name.startswith("radio_"):
            for inc in direct:
                if not (inc.startswith("proto_") or inc.startswith("radio_") or inc in RADIO_EXTRA_HEADERS):
                    violations.append(f"R4 {name}: radio file includes {inc}")

    return violations, warnings, len(files)


def main() -> int:
    if not COMPONENT_DIR.is_dir():
        raise SystemExit(f"error: component directory not found: {COMPONENT_DIR}")

    violations, warnings, file_count = check(COMPONENT_DIR)
    for line in warnings:
        print(line)
    for line in violations:
        print(line)
    if violations:
        print(f"include-graph: {len(violations)} violation(s)")
        return 1
    print(f"include-graph: OK ({file_count} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
