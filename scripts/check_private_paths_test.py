#!/usr/bin/env python3
"""Self-tests for scripts/check-private-paths.py.

Stdlib `assert`-based, no pytest, in-memory (path, text) fixtures -- same style as
scripts/check_key_material_test.py. The script under test is hyphen-named, so it is loaded via
importlib.util.spec_from_file_location.

Run via `python3 scripts/check_private_paths_test.py`; wired into `make private-path-check`.
"""

import importlib.util
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

_spec = importlib.util.spec_from_file_location("check_private_paths",
                                               REPO_ROOT / "scripts" / "check-private-paths.py")
cpp = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cpp)

# The fixtures are built from parts so this file's own text never needs the exemption to pass.
LAB = "lab" + "/"
ANALYSIS = "analysis" + "/"
REF = "reference" + "/"
AGENTS = "AGENTS" + ".md"


def test_hits_both_folder_names():
    text = f"ok line\n# see {LAB}sdr/run.md\nnote: ({ANALYSIS}issues/27.txt)\n"
    assert cpp.find_hits("tests/x.yaml", text) == [
        (2, f"# see {LAB}sdr/run.md"), (3, f"note: ({ANALYSIS}issues/27.txt)")]


def test_hit_at_line_start_and_in_backticks():
    assert len(cpp.find_hits("docs/a.md", f"{LAB}x\n`{ANALYSIS}y`")) == 2


def test_relative_and_absolute_paths_hit():
    text = (f"[x](../../{LAB}a.md)\n./{ANALYSIS}b\n"
            f"/home/u/dev/home_io_control/{LAB}c\n")
    assert len(cpp.find_hits("docs/a.md", text)) == 3


def test_plain_words_and_longer_paths_pass():
    text = ("a lab or synthetic device\nfurther analysis shows\n"
            f"hcio-{ANALYSIS}x\ngit{LAB}ci.yml\nsrc/{LAB}y\nfoo.{LAB}z\n")
    assert cpp.find_hits("docs/a.md", text) == []


def test_reference_checkout_paths_hit():
    text = (f"see {REF}iohomecontrol/include/a.h\n(`{REF}iown-homecontrol`)\n"
            f"../{REF}x\n/home/u/dev/home_io_control/{REF}y\n")
    assert len(cpp.find_hits("tests/x.yaml", text)) == 4


def test_reference_plain_words_pass():
    text = (f"reference or software-version code\nthe reference implementation\n"
            f"a {REF}\nsrc/{REF}x\nfoo.{REF}x\nmy-{REF}x\n")
    assert cpp.find_hits("docs/a.md", text) == []


def test_agents_file_hits():
    text = f"({AGENTS} / plan)\n`{AGENTS}`\n{AGENTS}\n"
    assert len(cpp.find_hits("config/x.yaml", text)) == 3
    assert cpp.find_hits("docs/a.md", f"SUBAGENTS{AGENTS}\nfoo-{AGENTS}\n") == []


def test_exempt_file_passes():
    assert cpp.find_hits("scripts/check-docs-prose.py", f"{ANALYSIS}") == []
    for rel in ("scripts/check-docs-prose.py", "scripts/check-docs-links.py",
                "scripts/check-key-material.py", "scripts/check-private-paths.py"):
        assert cpp.find_hits(rel, f"{REF}foo and {AGENTS}") == []


def test_allowlist_is_narrow():
    rel = "docs/a.md"
    allowed = f"the `{ANALYSIS}` sentence that is allowed"
    other = f"see `{ANALYSIS}foo.md`"
    saved = cpp.ALLOWLIST
    cpp.ALLOWLIST = frozenset({rel + ":sentence that is allowed"})
    try:
        assert cpp.find_hits(rel, allowed + "\n" + other) == [(2, other)]
        assert len(cpp.find_hits("docs/other.md", allowed)) == 1
    finally:
        cpp.ALLOWLIST = saved


def main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    for t in tests:
        t()
    print(f"check-private-paths self-tests OK ({len(tests)} cases).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
