#!/usr/bin/env python3
"""Self-tests for scripts/check-corpus-citations.py.

Stdlib `assert`-based, no pytest -- mirrors scripts/check_docs_links_test.py. The pure functions are
tested on small texts with a made-up corpus; the whole-repo listing runs against a throwaway git repo.

Run via `python3 scripts/check_corpus_citations_test.py`; wired into `make corpus-citation-check`.
"""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("check_corpus_citations", SCRIPTS / "check-corpus-citations.py")
ccc = importlib.util.module_from_spec(_spec)
sys.modules["check_corpus_citations"] = ccc
_spec.loader.exec_module(ccc)

# Built from parts: validate.py checks every literal capture path in a tracked file, and these name made-up captures.
EXCHANGE_DIR = str(ccc.CAPTURES_DIR / "exchange")

PATTERN = ccc.citation_pattern({"somfy_awning", "somfy_izymo_dimmer", "velux_klr200"}, ("exchange", "pairing"))
CAPTURES = frozenset({
    "somfy_awning_exchange_force_open_sx1276",
    "somfy_izymo_dimmer_pairing_full_sx1262",
    "somfy_izymo_dimmer_pairing_full_sx1276",
})


def broken(text: str, path: str = "docs/page.md", examples: dict | None = None) -> list[tuple[str, int, str]]:
    return ccc.broken_citations([(path, text)], CAPTURES, PATTERN, examples or {})


def test_an_existing_id_is_no_finding() -> None:
    assert broken("see somfy_awning_exchange_force_open_sx1276 for the bytes") == []


def test_a_missing_id_is_reported_with_its_file_and_line() -> None:
    text = "first line\n// as in somfy_izymo_dimmer_pairing_discover_confirm_sx1262\n"
    assert broken(text, "tests/x.cpp") == [("tests/x.cpp", 2, "somfy_izymo_dimmer_pairing_discover_confirm_sx1262")]


def test_a_generated_frame_name_cites_the_id_before_its_suffix() -> None:
    assert broken("somfy_awning_exchange_force_open_sx1276_frame7_bytes") == []
    assert broken("somfy_awning_exchange_force_open_sx1276_frame12") == []
    assert broken("somfy_awning_exchange_gone_frame3_bytes") == [("docs/page.md", 1, "somfy_awning_exchange_gone")]


def test_a_family_prefix_names_existing_captures() -> None:
    assert broken("the somfy_izymo_dimmer_pairing_full captures") == []
    assert broken("somfy_izymo_dimmer_pairing_ful") == [("docs/page.md", 1, "somfy_izymo_dimmer_pairing_ful")], (
        "a prefix is a family only at a token boundary")


def test_an_id_before_yaml_or_inside_a_path_is_checked() -> None:
    assert broken(f"{EXCHANGE_DIR}/somfy_awning_exchange_force_open_sx1276.yaml") == []
    assert broken(f"{EXCHANGE_DIR}/somfy_awning_exchange_gone_sx1276.yaml") == [
        ("docs/page.md", 1, "somfy_awning_exchange_gone_sx1276")]


def test_the_stem_of_a_glob_or_brace_pattern_must_name_a_family() -> None:
    assert broken("somfy_izymo_dimmer_pairing_full_{sx1262,sx1276}.yaml and somfy_izymo_dimmer_pairing_full_*") == []
    assert broken("somfy_izymo_dimmer_pairing_half_*") == [("docs/page.md", 1, "somfy_izymo_dimmer_pairing_half")]
    assert broken("somfy_izymo_dimmer_pairing_half_{sx1262,sx1276}") == [
        ("docs/page.md", 1, "somfy_izymo_dimmer_pairing_half")]


def test_text_without_a_subject_and_phase_cites_nothing() -> None:
    assert broken("somfy_awning_position, exchange_engine.cpp, velux_klr200 remote") == []
    assert broken("my_somfy_awning_exchange_x") == [], "an id starts at a word boundary, not inside a longer name"


def test_an_example_id_is_allowed_only_in_its_own_file() -> None:
    examples = {"velux_klr200_exchange_example": frozenset({"scripts/tool.py"})}
    assert broken("velux_klr200_exchange_example", "scripts/tool.py", examples) == []
    assert broken("velux_klr200_exchange_example", "docs/page.md", examples) == [
        ("docs/page.md", 1, "velux_klr200_exchange_example")]


def test_only_tracked_files_are_read() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        subprocess.run(["git", "init", "-q"], cwd=root, check=True)
        (root / EXCHANGE_DIR).mkdir(parents=True)
        (root / EXCHANGE_DIR / "somfy_awning_exchange_a.yaml").write_text("id: x\n")
        (root / "tracked.md").write_text("somfy_awning_exchange_b\n")
        (root / "untracked.md").write_text("somfy_awning_exchange_c\n")
        (root / "image.png").write_bytes(b"\x89PNG somfy_awning_exchange_d")
        (root / "scripts").mkdir()
        for own in ccc.OWN_FILES:
            (root / own).write_text("somfy_awning_exchange_made_up\n")
        subprocess.run(["git", "add", "tests", "tracked.md", "image.png", "scripts"], cwd=root, check=True)
        names = dict(ccc.tracked_text_files(root))
        assert "tracked.md" in names and "untracked.md" not in names and "image.png" not in names
        assert not set(names) & ccc.OWN_FILES
        assert ccc.existing_captures(root) == frozenset({"somfy_awning_exchange_a"})


def main() -> int:
    tests = [(n, f) for n, f in sorted(globals().items()) if n.startswith("test_") and callable(f)]
    failures = 0
    for name, fn in tests:
        try:
            fn()
        except AssertionError as exc:
            failures += 1
            print(f"FAIL {name}: {exc}")
    if failures:
        print(f"check_corpus_citations_test: {failures} of {len(tests)} failed")
        return 1
    print(f"check_corpus_citations_test: OK ({len(tests)} tests)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
