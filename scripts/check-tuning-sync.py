#!/usr/bin/env python3
"""Cross-language sync check for the tuning parameter registry.

The tuning parameters exist in two places that must agree:
  * Python: components/home_io_control/tuning.py — ``_NUMBER_PARAMS`` and
    ``_SELECT_OPTIONS`` dicts (keys are ``CONF_*`` string constants).
  * C++: components/home_io_control/tuning_registry.cpp — the ``NUMBER_PARAMS`` and
    ``SELECT_PARAMS`` tables, whose first field is the parameter name string.

This script extracts both name sets (parsing Python with ``ast`` — never importing
esphome — and the C++ tables with a regex) and exits non-zero with a diff if they
disagree. Run via ``make tuning-sync``; it is part of the ``lint`` composite target.

Adding a parameter means adding one C++ table row and one tuning.py entry; this guard
fails loudly if only one side is updated.
"""

import ast
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
TUNING_PY = REPO_ROOT / "components" / "home_io_control" / "tuning.py"
TUNING_REGISTRY_CPP = REPO_ROOT / "components" / "home_io_control" / "tuning_registry.cpp"


def _string_constants(module: ast.Module) -> dict:
    """Map top-level ``NAME = "literal"`` assignments to their string values."""
    constants = {}
    for node in module.body:
        if isinstance(node, ast.Assign) and isinstance(node.value, ast.Constant):
            if isinstance(node.value.value, str):
                for target in node.targets:
                    if isinstance(target, ast.Name):
                        constants[target.id] = node.value.value
    return constants


def _dict_keys(module: ast.Module, dict_name: str) -> list:
    """Return the key AST nodes of a top-level ``dict_name = { ... }`` assignment."""
    for node in module.body:
        if isinstance(node, ast.Assign) and isinstance(node.value, ast.Dict):
            for target in node.targets:
                if isinstance(target, ast.Name) and target.id == dict_name:
                    return node.value.keys
    raise SystemExit(f"error: could not find dict '{dict_name}' in {TUNING_PY}")


def _resolve_keys(keys: list, constants: dict) -> set:
    """Resolve dict key nodes (bare ``CONF_*`` names or string literals) to strings."""
    resolved = set()
    for key in keys:
        if isinstance(key, ast.Name):
            if key.id not in constants:
                raise SystemExit(f"error: key constant '{key.id}' has no string value in {TUNING_PY}")
            resolved.add(constants[key.id])
        elif isinstance(key, ast.Constant) and isinstance(key.value, str):
            resolved.add(key.value)
        else:
            raise SystemExit(f"error: unexpected dict key node {ast.dump(key)} in {TUNING_PY}")
    return resolved


def python_param_names() -> "tuple[set, set]":
    module = ast.parse(TUNING_PY.read_text(encoding="utf-8"))
    constants = _string_constants(module)
    numbers = _resolve_keys(_dict_keys(module, "_NUMBER_PARAMS"), constants)
    selects = _resolve_keys(_dict_keys(module, "_SELECT_OPTIONS"), constants)
    return numbers, selects


# Match a C++ table definition block: `... kName[] = { ... };` (non-greedy, DOTALL).
_TABLE_RE_TEMPLATE = r"{name}\s*\[\]\s*=\s*\{{(?P<body>.*?)\n\}};"
# Match the leading name string of each table row: `{"param_name",`.
_ROW_NAME_RE = re.compile(r'\{\s*"([^"]+)"')


def _cpp_table_names(source: str, table_name: str) -> set:
    match = re.search(_TABLE_RE_TEMPLATE.format(name=re.escape(table_name)), source, re.DOTALL)
    if not match:
        raise SystemExit(f"error: could not find C++ table '{table_name}' in {TUNING_REGISTRY_CPP}")
    return set(_ROW_NAME_RE.findall(match.group("body")))


def cpp_param_names() -> "tuple[set, set]":
    source = TUNING_REGISTRY_CPP.read_text(encoding="utf-8")
    return _cpp_table_names(source, "NUMBER_PARAMS"), _cpp_table_names(source, "SELECT_PARAMS")


def _report(kind: str, py_names: set, cpp_names: set) -> bool:
    if py_names == cpp_names:
        return True
    print(f"tuning-sync: {kind} parameter mismatch between tuning.py and tuning_registry.cpp", file=sys.stderr)
    only_py = sorted(py_names - cpp_names)
    only_cpp = sorted(cpp_names - py_names)
    if only_py:
        print(f"  only in tuning.py:            {only_py}", file=sys.stderr)
    if only_cpp:
        print(f"  only in tuning_registry.cpp:  {only_cpp}", file=sys.stderr)
    return False


def main() -> int:
    py_numbers, py_selects = python_param_names()
    cpp_numbers, cpp_selects = cpp_param_names()

    ok = _report("number", py_numbers, cpp_numbers)
    ok = _report("select", py_selects, cpp_selects) and ok

    if ok:
        total = len(cpp_numbers) + len(cpp_selects)
        print(f"tuning-sync: OK ({len(cpp_numbers)} number + {len(cpp_selects)} select = {total} parameters in sync)")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
