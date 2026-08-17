#!/usr/bin/env python3
"""Cross-language sync check for this component's hand-built paste-ready YAML emitters, and for
the two named-option tables' documented copies.

Three C++ functions build a YAML snippet as a plain string, for a user to copy into their own
config: build_oneway_adoption_report() and build_key_extraction_report() (both
components/home_io_control/hub_internal.h) and build_device_yaml_snippet()
(components/home_io_control/proto_device_model.cpp). Each one's emitted key names must track a
Python schema (ONEWAY_CONTROLLER_SCHEMA, the hub's own CONFIG_SCHEMA, and the four device-bound
platform schemas respectively) by hand -- nothing else keeps the two in step, so a schema key
rename or a newly-required key drifts silently until a user's paste fails to validate.

Separately, DEVICE_TYPE_OPTIONS and MANUFACTURER_OPTIONS (__init__.py) are each hand-transcribed a
second time as a markdown table in docs/home_io_control.md, for users picking a name. Nothing else
keeps *that* copy honest either, and it is arguably the worst of the three places for one to drift:
it is what a user actually copies, and a wrong byte here produces no error and no symptom -- 1W
has no reply to reveal it, and 2W devices simply advertise whichever manufacturer they really are
regardless of what a wrong docs table claims.

Same problem shape as scripts/check-tuning-sync.py, solved the same way: parse the Python side
with ``ast`` (never importing esphome -- ONEWAY_CONTROLLER_SCHEMA et al. only import cleanly
inside ESPHome's own component loader, which is not installed on the host) and the other side
(C++ string literals, or the docs markdown) with a regex, then diff.

Run via ``make yaml-emitter-sync``; it is part of the ``lint`` composite target.
"""

import ast
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
COMPONENT_DIR = REPO_ROOT / "components" / "home_io_control"

INIT_PY = COMPONENT_DIR / "__init__.py"
PLATFORM_COMMON_PY = COMPONENT_DIR / "platform_common.py"
COVER_PY = COMPONENT_DIR / "cover.py"
LIGHT_PY = COMPONENT_DIR / "light.py"
SWITCH_PY = COMPONENT_DIR / "switch.py"
LOCK_PY = COMPONENT_DIR / "lock.py"
PLATFORM_PY_FILES = [PLATFORM_COMMON_PY, COVER_PY, LIGHT_PY, SWITCH_PY, LOCK_PY]

HUB_INTERNAL_H = COMPONENT_DIR / "hub_internal.h"
PROTO_DEVICE_MODEL_CPP = COMPONENT_DIR / "proto_device_model.cpp"

DOCS_MD = REPO_ROOT / "docs" / "home_io_control.md"

# esphome.const names used as bare schema-key arguments in the files above that are not local
# `CONF_X = "..."` string constants (so _string_constants() below cannot resolve them from any
# single file's own AST) -- both are long-stable ESPHome fundamentals, not project-specific.
ESPHOME_CONST_FALLBACK = {"CONF_ID": "id", "CONF_NAME": "name"}


# =====================================================================================
# Python side: schema key extraction (ast-parsed, no esphome import)
# =====================================================================================


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


def _load_constants(files: "list[Path]") -> dict:
    constants = dict(ESPHOME_CONST_FALLBACK)
    for path in files:
        constants.update(_string_constants(ast.parse(path.read_text(encoding="utf-8"))))
    return constants


def _resolve_key_name(key_node: ast.AST, constants: dict) -> "tuple[str, bool] | None":
    """Resolve one schema dict key node to (key_name, is_required).

    Keys are ``cv.Required(CONF_X)`` / ``cv.Optional(CONF_X, ...)`` / ``cv.GenerateID(CONF_X)``
    calls, or bare string literals. A bare ``cv.GenerateID()`` (no argument) names no checkable
    field -- it is the entity's own auto-generated ``id:``, unrelated to this project's keys --
    and resolves to None, same as any name this pass cannot resolve.
    """
    if isinstance(key_node, ast.Constant) and isinstance(key_node.value, str):
        return key_node.value, False
    if isinstance(key_node, ast.Call) and isinstance(key_node.func, ast.Attribute):
        method = key_node.func.attr
        if method not in ("Required", "Optional", "GenerateID") or not key_node.args:
            return None
        arg = key_node.args[0]
        if isinstance(arg, ast.Constant) and isinstance(arg.value, str):
            return arg.value, method == "Required"
        if isinstance(arg, ast.Name) and arg.id in constants:
            return constants[arg.id], method == "Required"
    return None


def _schema_keys_from_dicts(dict_nodes: "list[ast.Dict]", constants: dict) -> dict:
    """Return {key_name: is_required}, OR-ing the required flag across every dict node given."""
    keys: dict = {}
    for dict_node in dict_nodes:
        for key_node in dict_node.keys:
            resolved = _resolve_key_name(key_node, constants)
            if resolved is None:
                continue
            name, required = resolved
            keys[name] = keys.get(name, False) or required
    return keys


def _dicts_in_named_assignment(module: ast.Module, name: str, path: Path) -> "list[ast.Dict]":
    """Every ast.Dict literal within the RHS subtree of a top-level ``name = <expr>``.

    Used for __init__.py, which defines several unrelated schemas in one file
    (ONEWAY_CONTROLLER_SCHEMA, the hub's own CONFIG_SCHEMA, the LR1121 firmware-update schema) --
    a whole-module walk would merge all of them, which is too loose for telling one emitter's
    keys apart from another's.
    """
    for node in module.body:
        if isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name) and target.id == name:
                    return [n for n in ast.walk(node.value) if isinstance(n, ast.Dict)]
    raise SystemExit(f"error: could not find top-level assignment '{name}' in {path}")


def oneway_controller_schema_keys() -> dict:
    module = ast.parse(INIT_PY.read_text(encoding="utf-8"))
    constants = _load_constants([INIT_PY])
    return _schema_keys_from_dicts(_dicts_in_named_assignment(module, "ONEWAY_CONTROLLER_SCHEMA", INIT_PY), constants)


def hub_config_schema_keys() -> dict:
    module = ast.parse(INIT_PY.read_text(encoding="utf-8"))
    constants = _load_constants([INIT_PY])
    return _schema_keys_from_dicts(_dicts_in_named_assignment(module, "CONFIG_SCHEMA", INIT_PY), constants)


def device_platform_schema_keys() -> dict:
    """Union of platform_schema_extension() plus each device-bound platform's own extra keys.

    A whole-module dict walk (rather than the named-assignment lookup above) because
    light.py's CONFIG_SCHEMA builds its dict inside a helper function (_validate()), not in a
    top-level `CONFIG_SCHEMA = ...` assignment the way cover.py/switch.py/lock.py do -- each of
    these five files is narrowly scoped to one platform's schema, so a whole-file walk carries
    none of __init__.py's multi-schema cross-contamination risk.
    """
    constants = _load_constants([INIT_PY] + PLATFORM_PY_FILES)
    keys: dict = {}
    for path in PLATFORM_PY_FILES:
        module = ast.parse(path.read_text(encoding="utf-8"))
        dict_nodes = [n for n in ast.walk(module) if isinstance(n, ast.Dict)]
        for name, required in _schema_keys_from_dicts(dict_nodes, constants).items():
            keys[name] = keys.get(name, False) or required
    return keys


def _named_int_dict(path: Path, name: str) -> dict:
    """Return {str_key: int_value} for a top-level ``name = {"key": 0xNN, ...}`` dict literal.

    Unlike the schema dicts above, DEVICE_TYPE_OPTIONS/MANUFACTURER_OPTIONS map straight to
    integer literals, not cv.Required(...)/cv.Optional(...) calls -- a plain ast.Dict walk with
    no key-resolution step needed.
    """
    module = ast.parse(path.read_text(encoding="utf-8"))
    for node in module.body:
        if isinstance(node, ast.Assign) and isinstance(node.value, ast.Dict):
            for target in node.targets:
                if isinstance(target, ast.Name) and target.id == name:
                    result = {}
                    for key_node, value_node in zip(node.value.keys, node.value.values):
                        if (
                            isinstance(key_node, ast.Constant)
                            and isinstance(key_node.value, str)
                            and isinstance(value_node, ast.Constant)
                            and isinstance(value_node.value, int)
                        ):
                            result[key_node.value] = value_node.value
                    return result
    raise SystemExit(f"error: could not find top-level dict '{name}' in {path}")


# =====================================================================================
# Docs side: markdown table extraction
# =====================================================================================

# A `| `name` | `0xNN` |` cell pair, as used by both "Named device types" and "Named
# manufacturers" (docs/home_io_control.md) -- each table is two of these pairs per row.
_DOCS_TABLE_CELL_RE = re.compile(r"\|\s*`([a-z][a-z0-9_]*)`\s*\|\s*`(0x[0-9A-Fa-f]+)`\s*")


def _parse_docs_table(text: str, heading: str, path: Path) -> dict:
    """Parse every ``| `name` | `0xNN` |`` cell pair out of the markdown section starting at
    `heading`, up to (not including) the next `##`/`###` heading.
    """
    start = text.find(heading)
    if start == -1:
        raise SystemExit(f"error: could not find docs heading {heading!r} in {path}")
    body_start = start + len(heading)
    next_heading = re.search(r"\n#{2,3} ", text[body_start:])
    section = text[body_start : body_start + next_heading.start()] if next_heading else text[body_start:]
    return {name: int(hexval, 16) for name, hexval in _DOCS_TABLE_CELL_RE.findall(section)}


# =====================================================================================
# C++ side: emitted-key extraction (regex over each emitter's own source text)
# =====================================================================================

_STRING_LITERAL_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
# A YAML key line, optionally as a `- key:` list item; deliberately requires a lowercase first
# character so prose sentences beginning with an uppercase word (e.g. "MAC VERIFIED: ...", part
# of build_oneway_adoption_report()'s explanatory text) can never be mistaken for a key.
_KEY_LINE_RE = re.compile(r"^-?\s*([a-z][a-z0-9_]*):")


def _cpp_unescape(literal: str) -> str:
    return literal.replace('\\n', '\n').replace('\\"', '"').replace('\\\\', '\\')


def _strip_cpp_comments(text: str) -> str:
    """Remove `//` and `/* */` comments, respecting string literals.

    Without this, a doc comment that quotes a YAML key for illustration (e.g. `manufacturer_line`'s
    own "Fits "    manufacturer: 0xNN" plus its terminator..." comment, right above its snprintf()
    call) is indistinguishable from a real string literal to a plain regex scan, and gets
    misread as emitted content.
    """
    out = []
    in_string = False
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if in_string:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == '"':
                in_string = False
            i += 1
            continue
        if c == '"':
            in_string = True
            out.append(c)
            i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j == -1 else j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j == -1 else j + 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def _extract_function_body(source: str, func_name: str, path: Path) -> str:
    """Return the brace-matched body text of `func_name`'s definition in `source`.

    Plain depth-counting over raw characters, with no awareness of string-literal-internal
    braces -- safe for the three functions this script targets, none of which put a literal `{`
    or `}` inside their own string content. A future edit that added one would make this
    over/under-match, which fails loudly (missing/garbled keys) rather than silently, an
    acceptable trade for staying dependency-light like check-tuning-sync.py.
    """
    # Anchored on the return type, not just the bare name: several of these functions are also
    # mentioned by name in doxygen prose elsewhere in the same file (e.g. "see
    # build_oneway_adoption_report() above"), and a bare-name search finds whichever comes first
    # in the file, definition or not -- silently brace-matching from the wrong `{` entirely.
    match = re.search(r"std::string\s+" + re.escape(func_name) + r"\s*\(", source)
    if not match:
        raise SystemExit(f"error: could not find function '{func_name}' in {path}")
    brace_start = source.index("{", match.end())
    depth = 0
    for i in range(brace_start, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[brace_start : i + 1]
    raise SystemExit(f"error: unbalanced braces scanning '{func_name}' in {path}")


def emitted_keys(cpp_path: Path, func_name: str) -> set:
    """Every YAML key name findable in `func_name`'s own string-literal content.

    Checks each quoted string literal within the function body *independently*, never joined
    with its neighbours. Joining in source order was tried and is wrong: some fields are built
    through a local variable assigned by a separate, earlier statement (manufacturer_line via
    snprintf(), type_lines via an if/else) rather than inline in the final `return`, so the
    literal that is logically adjacent to it at runtime (the "\\n" separator in
    `manufacturer_line + "\\n" + type_lines`) is not textually adjacent to it in the source --
    joining glued unrelated fragments onto the same reconstructed line and hid the key that
    followed. Every key this project's emitters write is instead the *first* thing in its own
    literal fragment, by convention (`"    io_device_type: " + ...`), so checking each fragment
    on its own, split by its own internal newlines, finds every real key without needing to
    reconstruct concatenation order at all.

    Two shapes coexisting in one function (a complete emission and an alternate one commented out
    in the source, e.g. build_oneway_adoption_report()'s unknown-device-class fallback) collapse
    naturally this way too: the commented alternative's own lines start with `#` and are filtered
    below, so the result already matches the *complete* shape's real key set.
    """
    body = _strip_cpp_comments(_extract_function_body(cpp_path.read_text(encoding="utf-8"), func_name, cpp_path))
    keys = set()
    for raw_literal in _STRING_LITERAL_RE.findall(body):
        for line in _cpp_unescape(raw_literal).split("\n"):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            match = _KEY_LINE_RE.match(line)
            if match:
                keys.add(match.group(1))
    return keys


# =====================================================================================
# Cross-check
# =====================================================================================


class Emitter:
    def __init__(self, label, cpp_path, func_name, schema_fn, skip_keys, check_required, note=""):
        self.label = label
        self.cpp_path = cpp_path
        self.func_name = func_name
        self.schema_fn = schema_fn
        self.skip_keys = skip_keys
        self.check_required = check_required
        self.note = note


EMITTERS = [
    Emitter(
        "build_oneway_adoption_report -> ONEWAY_CONTROLLER_SCHEMA",
        HUB_INTERNAL_H,
        "build_oneway_adoption_report",
        oneway_controller_schema_keys,
        skip_keys={"oneway_controllers"},  # the block's own wrapping key, not a schema field
        check_required=True,
    ),
    Emitter(
        "build_key_extraction_report -> hub CONFIG_SCHEMA",
        HUB_INTERNAL_H,
        "build_key_extraction_report",
        hub_config_schema_keys,
        skip_keys={"home_io_control"},  # the block's own wrapping key, not a schema field
        # Deliberately partial: this emits only the two recovered secret fields (node_id,
        # system_key), meant to be merged into an *existing* hub config -- unlike the other two
        # emitters, it was never meant to satisfy every Required key of its target schema
        # (rst_pin, radio_type, ... are required but never part of this snippet by design).
        check_required=False,
    ),
    Emitter(
        "build_device_yaml_snippet -> device-bound platform schemas",
        PROTO_DEVICE_MODEL_CPP,
        "build_device_yaml_snippet",
        device_platform_schema_keys,
        # "platform" is ESPHome's own platform-selection key (always "home_io_control" here),
        # injected by ESPHome's platform-component machinery -- not one of this project's own
        # schema keys. The domain line itself (e.g. "cover:") and the "<...>" placeholder both
        # produce no capturable key on their own -- see _KEY_LINE_RE -- so need no skip entry.
        skip_keys={"platform"},
        check_required=True,
    ),
]


def _check_emitter(emitter: Emitter) -> bool:
    schema_keys = emitter.schema_fn()
    emitted = emitted_keys(emitter.cpp_path, emitter.func_name) - emitter.skip_keys

    ok = True
    unknown = sorted(k for k in emitted if k not in schema_keys)
    if unknown:
        ok = False
        print(f"yaml-emitter-sync: {emitter.label}: emits key(s) not in the schema: {unknown}", file=sys.stderr)

    if emitter.check_required:
        missing_required = sorted(k for k, required in schema_keys.items() if required and k not in emitted)
        if missing_required:
            ok = False
            print(
                f"yaml-emitter-sync: {emitter.label}: schema requires key(s) the emitter never writes: "
                f"{missing_required}",
                file=sys.stderr,
            )

    return ok


class DocsTable:
    def __init__(self, label, python_dict_name, heading, exclude=frozenset()):
        self.label = label
        self.python_dict_name = python_dict_name
        self.heading = heading
        # Entries deliberately not offered as a name in the docs table (e.g. a sentinel value),
        # so their absence there is not drift.
        self.exclude = exclude


DOCS_TABLES = [
    DocsTable(
        "DEVICE_TYPE_OPTIONS -> docs \"Named device types\" table",
        "DEVICE_TYPE_OPTIONS",
        "### Named device types",
        # "unknown" is the schema's absent-type sentinel (0x00), not a real device class a user
        # would pick from a table of named types -- deliberately not listed there.
        exclude={"unknown"},
    ),
    DocsTable(
        "MANUFACTURER_OPTIONS -> docs \"Named manufacturers\" table",
        "MANUFACTURER_OPTIONS",
        "### Named manufacturers",
    ),
]


def _check_docs_table(table: DocsTable, docs_text: str) -> bool:
    python_entries = _named_int_dict(INIT_PY, table.python_dict_name)
    for key in table.exclude:
        python_entries.pop(key, None)
    docs_entries = _parse_docs_table(docs_text, table.heading, DOCS_MD)

    ok = True
    missing = sorted(set(python_entries) - set(docs_entries))
    if missing:
        ok = False
        print(f"yaml-emitter-sync: {table.label}: missing from the docs table: {missing}", file=sys.stderr)
    extra = sorted(set(docs_entries) - set(python_entries))
    if extra:
        ok = False
        print(
            f"yaml-emitter-sync: {table.label}: docs table has entries {table.python_dict_name} does not: {extra}",
            file=sys.stderr,
        )
    for name in sorted(set(python_entries) & set(docs_entries)):
        if python_entries[name] != docs_entries[name]:
            ok = False
            print(
                f"yaml-emitter-sync: {table.label}: \"{name}\" = 0x{python_entries[name]:02X} in "
                f"{table.python_dict_name} but 0x{docs_entries[name]:02X} in the docs table.",
                file=sys.stderr,
            )
    return ok


def main() -> int:
    results = [_check_emitter(emitter) for emitter in EMITTERS]

    docs_text = DOCS_MD.read_text(encoding="utf-8")
    results += [_check_docs_table(table, docs_text) for table in DOCS_TABLES]

    if all(results):
        print(
            f"yaml-emitter-sync: OK ({len(EMITTERS)} emitters and {len(DOCS_TABLES)} docs tables "
            "in sync with their schemas)"
        )
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
