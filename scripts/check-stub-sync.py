#!/usr/bin/env python3
"""Signature sync check between the host-test ESPHome stubs and the real ESPHome headers.

The host unit tests build against hand-written stubs of the ESPHome API (``tests/include/esphome``,
ADR 0014) instead of ESPHome itself. A stub that drifts from the real header can keep host tests
green while the firmware behaves differently: an overload the real API lacks, a parameter of a
different type, a member of a different width. This script pins the part of the surface the
component uses: for every row in ``ROWS`` it finds the named class in both the stub header and the
real header, extracts the declarations of the listed members, and checks:

  1. the member exists in the stub and in the real header (a row that matches nothing on either
     side fails, so a renamed class or a matcher gap can never pass silently);
  2. every stub overload has a real overload with the same return type, parameter types and
     const-qualification. A real overload whose trailing parameters have defaults also matches a
     stub that omits them, since the same calls compile against both;
  3. a pinned data member has the same type on both sides.

Real overloads the stub does not declare are fine: the stubs deliberately model only what the
component calls. A deliberate deviation is listed in ``ALLOWED_DEVIATIONS`` with its reason.

Runs inside the ESPHome image, where ``import esphome`` locates the pinned headers: use
``make stub-sync`` (part of ``make lint``). ``--verbose`` prints every stub declaration with the real
one it matched. Adding a row is how a newly stubbed API gets pinned.
"""

import re
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
STUB_ROOT = REPO_ROOT / "tests" / "include" / "esphome"


@dataclass(frozen=True)
class Row:
    """One stubbed class: where it lives on each side, and which of its members are pinned."""

    stub_header: str  # relative to tests/include/esphome
    real_header: str  # relative to the esphome package
    stub_class: str
    methods: tuple = ()
    members: tuple = ()
    real_class: str = ""  # defaults to stub_class

    @property
    def label(self) -> str:
        return self.stub_class


ROWS = (
    Row(
        "core/component.h",
        "core/component.h",
        "Component",
        methods=(
            "set_timeout",
            "set_interval",
            "cancel_timeout",
            "cancel_interval",
            "setup",
            "loop",
            "dump_config",
            "get_setup_priority",
            "mark_failed",
            "is_failed",
        ),
        members=("warn_if_blocking_over_",),
    ),
    Row(
        "core/application.h",
        "core/scheduler.h",
        "Scheduler",
        methods=("set_timeout", "set_interval", "cancel_timeout", "cancel_interval"),
    ),
    Row("core/application.h", "core/application.h", "Application", methods=("feed_wdt", "safe_reboot")),
    Row("core/preferences.h", "core/preference_backend.h", "ESPPreferenceObject", methods=("save", "load")),
    Row("components/cover/cover.h", "components/cover/cover.h", "Cover", methods=("control", "get_traits", "publish_state")),
    Row(
        "components/light/light_output.h",
        "components/light/light_output.h",
        "LightOutput",
        methods=("setup_state", "write_state", "get_traits"),
    ),
    Row("components/switch/switch.h", "components/switch/switch.h", "Switch", methods=("write_state", "publish_state")),
    Row(
        "components/binary_sensor/binary_sensor.h",
        "components/binary_sensor/binary_sensor.h",
        "BinarySensor",
        methods=("publish_state",),
        members=("state",),
    ),
    Row("components/lock/lock.h", "components/lock/lock.h", "Lock", methods=("control", "publish_state")),
    Row(
        "components/climate/climate.h",
        "components/climate/climate.h",
        "Climate",
        methods=("control", "traits", "publish_state"),
    ),
    Row("components/button/button.h", "components/button/button.h", "Button", methods=("press_action",)),
    Row("components/sensor/sensor.h", "components/sensor/sensor.h", "Sensor", methods=("publish_state",)),
    Row(
        "components/text_sensor/text_sensor.h",
        "components/text_sensor/text_sensor.h",
        "TextSensor",
        methods=("publish_state",),
    ),
    Row("components/number/number.h", "components/number/number.h", "Number", methods=("control", "publish_state")),
    Row("components/select/select.h", "components/select/select.h", "Select", methods=("control", "publish_state")),
)

# (class, stub signature as printed by this script) -> why the stub deliberately differs.
ALLOWED_DEVIATIONS = {
    ("Component", "void mark_failed(const char *)"): (
        "ESPHome takes `const LogString *` (LOG_STR()); on host LOG_STR() resolves to a plain string, "
        "so the stub takes that instead of modelling the PROGMEM plumbing"
    ),
}

# === Declaration extraction ===

_KEYWORDS_DROPPED_FROM_RETURN = {"virtual", "inline", "static", "explicit", "constexpr", "friend"}
_TYPE_WORDS = {
    "const", "volatile", "unsigned", "signed", "short", "long", "int", "char", "bool", "float",
    "double", "void", "auto", "size_t", "struct", "class", "enum", "typename",
}  # fmt: skip


@dataclass
class Decl:
    ret: str
    name: str
    params: list  # list of (type, has_default)
    const: bool
    template: str = ""

    def signature(self) -> str:
        params = ", ".join(t for t, _ in self.params)
        prefix = f"{self.template} " if self.template else ""
        return f"{prefix}{self.ret} {self.name}({params}){' const' if self.const else ''}".strip()


def strip_comments(source: str) -> str:
    """`source` without comments and preprocessor lines (both sides of an #if are kept)."""
    source = re.sub(r"/\*.*?\*/", " ", source, flags=re.S)
    source = re.sub(r"//[^\n]*", " ", source)
    return re.sub(r"^[ \t]*#[^\n]*(?:\\\n[^\n]*)*", " ", source, flags=re.M)


def matching(source: str, start: int, open_ch: str, close_ch: str) -> int:
    """Index of the bracket closing the one at `start`."""
    depth = 0
    for index in range(start, len(source)):
        if source[index] == open_ch:
            depth += 1
        elif source[index] == close_ch:
            depth -= 1
            if depth == 0:
                return index
    raise ValueError(f"unbalanced {open_ch}{close_ch}")


def class_body(source: str, class_name: str) -> "str | None":
    """Body of the first *definition* of `class_name` (a forward declaration has no body)."""
    for match in re.finditer(rf"\b(?:class|struct)\s+{re.escape(class_name)}\b[^;{{]*\{{", source):
        brace = match.end() - 1
        return source[brace + 1 : matching(source, brace, "{", "}")]
    return None


def top_level(body: str) -> str:
    """`body` with every nested {...} block blanked, so only the class's own declarations remain."""
    out = []
    depth = 0
    for ch in body:
        if ch == "{":
            depth += 1
            out.append(";" if depth == 1 else " ")
            continue
        if ch == "}":
            depth -= 1
            out.append(" ")
            continue
        out.append(ch if depth == 0 else " ")
    return "".join(out)


def split_top_level_commas(text: str) -> list:
    parts, depth, current = [], 0, []
    for ch in text:
        if ch in "<([":
            depth += 1
        elif ch in ">)]":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(current))
            current = []
        else:
            current.append(ch)
    if "".join(current).strip():
        parts.append("".join(current))
    return parts


def normalize_type(text: str) -> str:
    text = re.sub(r"\s+", " ", text).strip()
    text = re.sub(r"\s*([*&]+)\s*", r" \1", text)  # "char*x" / "char *" -> "char *"
    text = re.sub(r"\s*::\s*", "::", text)
    text = re.sub(r"\s*<\s*", "<", text)
    text = re.sub(r"\s*>", ">", text)
    text = re.sub(r"\(\s*\)", "()", text)
    text = re.sub(r"\bclass\s+|\bstruct\s+", "", text)  # elaborated type specifiers
    return text.strip()


def normalize_param(text: str) -> "tuple[str, bool]":
    has_default = "=" in text
    text = text.split("=", 1)[0].strip()
    tokens = re.findall(r"[A-Za-z_][\w:]*(?:<[^()]*?>)?(?:\(\))?|[*&]+|[<>(),]", text)
    # Drop a trailing parameter name: an identifier after a complete type.
    if len(tokens) >= 2 and re.fullmatch(r"[A-Za-z_]\w*", tokens[-1]) and tokens[-1] not in _TYPE_WORDS:
        text = text[: text.rfind(tokens[-1])]
    return normalize_type(text), has_default


def declaration_head(text: str) -> str:
    """The part of a declaration before its name, without access specifiers or attributes."""
    text = re.sub(r"\b(public|protected|private)\s*:", " ", text)
    return re.sub(r"\[\[[^\]]*\]\]", " ", text)


def extract_methods(body: str, name: str) -> list:
    flat = top_level(body)
    decls = []
    for match in re.finditer(rf"(?<![\w~:.>]){re.escape(name)}\s*\(", flat):
        open_paren = match.end() - 1
        close_paren = matching(flat, open_paren, "(", ")")
        head_start = flat.rfind(";", 0, match.start())
        head = declaration_head(flat[head_start + 1 : match.start()])
        template = ""
        template_match = re.match(r"\s*(template\s*<.*>)\s*", head, flags=re.S)
        if template_match:
            template = normalize_type(template_match.group(1))
            template = re.sub(r"(typename|class) \w+", r"\1", template)
            head = head[template_match.end() :]
        words = [w for w in head.split() if w not in _KEYWORDS_DROPPED_FROM_RETURN]
        if not words:
            continue  # a call, not a declaration
        tail = flat[close_paren + 1 : close_paren + 80]
        params_text = flat[open_paren + 1 : close_paren].strip()
        params = [] if params_text in ("", "void") else [normalize_param(p) for p in split_top_level_commas(params_text)]
        decls.append(
            Decl(
                ret=normalize_type(" ".join(words)),
                name=name,
                params=params,
                const=bool(re.match(r"\s*const\b", tail)),
                template=template,
            )
        )
    return decls


def extract_member_type(body: str, name: str) -> "str | None":
    flat = top_level(body)
    match = re.search(rf"(?<![\w.>]){re.escape(name)}\s*[;=]", flat)
    if not match:
        return None
    head = declaration_head(flat[flat.rfind(";", 0, match.start()) + 1 : match.start()])
    return normalize_type(head) or None


def covers(real: Decl, stub: Decl) -> bool:
    """True when every call the stub overload accepts also resolves to `real` with the same types."""
    if (real.ret, real.const, real.template) != (stub.ret, stub.const, stub.template):
        return False
    if len(stub.params) > len(real.params):
        return False
    if [t for t, _ in real.params[: len(stub.params)]] != [t for t, _ in stub.params]:
        return False
    return all(has_default for _, has_default in real.params[len(stub.params) :])


# === Check ===


def esphome_root() -> Path:
    try:
        import esphome  # noqa: PLC0415
    except ImportError:
        print("check-stub-sync: `import esphome` failed. Run `make stub-sync` (it runs inside the ESPHome image).",
              file=sys.stderr)  # fmt: skip
        sys.exit(2)
    return Path(esphome.__file__).resolve().parent


def check_row(row: Row, real_root: Path, verbose: bool = False) -> list:
    errors = []
    stub_source = strip_comments((STUB_ROOT / row.stub_header).read_text(encoding="utf-8"))
    real_path = real_root / row.real_header
    if not real_path.is_file():
        return [f"{row.label}: real header {row.real_header} not found in the ESPHome package"]
    real_source = strip_comments(real_path.read_text(encoding="utf-8"))
    stub_body = class_body(stub_source, row.stub_class)
    real_body = class_body(real_source, row.real_class or row.stub_class)
    if stub_body is None:
        return [f"{row.label}: class not found in stub {row.stub_header}"]
    if real_body is None:
        return [f"{row.label}: class not found in real {row.real_header}: renamed or moved upstream?"]

    for method in row.methods:
        stub_decls = extract_methods(stub_body, method)
        real_decls = extract_methods(real_body, method)
        if not stub_decls:
            errors.append(f"{row.label}::{method}: no declaration in stub {row.stub_header} (fix the row or the matcher)")
        if not real_decls:
            errors.append(f"{row.label}::{method}: no declaration in real {row.real_header}: removed or renamed upstream?")
        if not stub_decls or not real_decls:
            continue
        for stub in stub_decls:
            match = next((real for real in real_decls if covers(real, stub)), None)
            if verbose:
                print(f"  {row.label}: {stub.signature()}  ->  {match.signature() if match else 'NO MATCH'}")
            if match is not None:
                continue
            if (row.label, stub.signature()) in ALLOWED_DEVIATIONS:
                continue
            real_list = "\n".join(f"        + {real.signature()}" for real in real_decls)
            errors.append(f"{row.label}::{method}: stub overload has no real counterpart\n"
                          f"        - {stub.signature()}   (stub {row.stub_header})\n{real_list}   (real {row.real_header})")  # fmt: skip

    for member in row.members:
        stub_type = extract_member_type(stub_body, member)
        real_type = extract_member_type(real_body, member)
        if stub_type is None or real_type is None:
            side = "stub" if stub_type is None else "real"
            errors.append(f"{row.label}::{member}: member not found in {side} header")
        elif verbose:
            print(f"  {row.label}: member {member}: stub {stub_type} / real {real_type}")
        if stub_type is not None and real_type is not None and stub_type != real_type:
            errors.append(f"{row.label}::{member}: type differs\n        - {stub_type}   (stub)\n        + {real_type}   (real)")
    return errors


def main() -> int:
    verbose = "--verbose" in sys.argv[1:]
    real_root = esphome_root()
    errors = []
    checked = 0
    for row in ROWS:
        errors.extend(check_row(row, real_root, verbose))
        checked += len(row.methods) + len(row.members)
    if errors:
        print(f"check-stub-sync: {len(errors)} drift(s) between tests/include/esphome and {real_root}:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    print(f"check-stub-sync: OK ({checked} pinned declarations across {len(ROWS)} classes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
