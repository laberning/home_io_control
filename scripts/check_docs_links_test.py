#!/usr/bin/env python3
"""Self-tests for the docs tooling: scripts/stage-docs.py and scripts/check-docs-links.py.

Stdlib `assert`-based, no pytest -- mirrors scripts/check_key_material_test.py. The pure
transforms are tested in memory; the whole-repo checks are run against a throwaway git repo
built in a tmpdir with the module globals repointed at it.

Both scripts under test are hyphen-named, so they are loaded via importlib.

Run via `python3 scripts/check_docs_links_test.py`; wired into `make docs-link-check`.
Exits non-zero on the first failure.
"""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPTS))


def _load(name: str, filename: str):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / filename)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


sd = _load("stage_docs", "stage-docs.py")
cdl = _load("check_docs_links", "check-docs-links.py")

FAKE = sd.REPO_ROOT / "docs" / "_fixture.md"  # a plausible path for error messages
_failures: list[str] = []


def check(cond: bool, msg: str) -> None:
    if not cond:
        _failures.append(msg)


# ---------------------------------------------------------------------------
# stage-docs.py — pure transforms
# ---------------------------------------------------------------------------

def test_label_injection() -> None:
    out = sd.inject_label("# Title\n<!-- doxygen-label: pg -->\n\nbody\n", FAKE)
    check(out.splitlines()[0] == "# Title {#pg}", "label appended to first H1")
    check(out.count("\n") == 4, "label injection is newline-neutral")

    check(sd.inject_label("# Plain\n\nbody\n", FAKE) == "# Plain\n\nbody\n", "no label -> unchanged")


def test_label_conflict_and_missing_h1() -> None:
    for text, why in [
        ("# T\n<!-- doxygen-label: a -->\n<!-- doxygen-label: b -->\n", "two different labels"),
        ("no heading\n<!-- doxygen-label: x -->\n", "label but no H1"),
    ]:
        try:
            sd.inject_label(text, FAKE)
            check(False, f"inject_label should exit on: {why}")
        except SystemExit:
            pass


def test_label_and_subpages_are_fence_aware() -> None:
    text = (
        "# Real\n<!-- doxygen-label: real -->\n\n"
        "```markdown\n<!-- doxygen-label: example_only -->\n```\n"
    )
    check(sd._label_for(text, FAKE) == "real", "label inside a fence is ignored")
    out = sd.inject_label(text, FAKE)
    check("<!-- doxygen-label: example_only -->" in out, "fenced example left intact")

    sp = (
        "# Idx\n<!-- doxygen-label: idx -->\n\n"
        "```markdown\n<!-- doxygen-subpages -->\n- [X](x.md)\n<!-- /doxygen-subpages -->\n```\n"
    )
    check("\\subpage" not in sd.generate_subpages(sp, FAKE), "subpages block inside a fence untouched")

    fenced_h1 = "```\n# fake\n```\n# Real H1\n<!-- doxygen-label: r -->\n"
    check("# Real H1 {#r}" in sd.inject_label(fenced_h1, FAKE), "H1 inside a fence is not the target")


def test_link_rewrites() -> None:
    check(
        sd.rewrite_links("[a](docs/home_io_control.md#x)") == "[a](build/docs/home_io_control.md#x)",
        "README docs/ -> build/docs/ boundary rewrite",
    )
    grp = "[Protocol](https://laberning.github.io/home_io_control/group__hioc__protocol.html)"
    check(sd.rewrite_links(grp) == r'\ref hioc_protocol "Protocol"', "group-page URL -> \\ref")
    fenced = "```\n[a](docs/x.md)\n```\n[b](docs/x.md)\n"
    r = sd.rewrite_links(fenced)
    check("```\n[a](docs/x.md)\n```" in r and "[b](build/docs/x.md)" in r, "rewrite skips fences")


def test_dest_path() -> None:
    R = sd.REPO_ROOT
    check(sd._dest_path(R / "README.md") == sd.DOCS_OUT_DIR / "README.md", "README -> build/docs/README.md")
    check(sd._dest_path(R / "docs/foo.md") == sd.DOCS_OUT_DIR / "foo.md", "docs/ prefix stripped")
    check(
        sd._dest_path(R / "docs/adr/0001.md") == sd.DOCS_OUT_DIR / "adr" / "0001.md",
        "sub-directory preserved",
    )


def test_stage_line_count_neutral() -> None:
    # Every real staged doc keeps its line count (the invariant `stage()` asserts).
    for src in sd._sources():
        raw = src.read_text(encoding="utf-8")
        check(
            sd.stage(src).count("\n") == raw.count("\n"),
            f"staging {src.name} changed the line count",
        )


# ---------------------------------------------------------------------------
# check-docs-links.py — helpers
# ---------------------------------------------------------------------------

def test_helpers() -> None:
    R = cdl.REPO_ROOT
    check(cdl._is_published_path(R / "README.md"), "README is published")
    check(cdl._is_published_path(R / "docs/adr/0001-x.md"), "docs/** is published")
    check(not cdl._is_published_path(R / "tests/corpus/README.md"), "tests/ is not published")

    # The real Doxyfile names the staging root as a directory and nothing else; the per-file form
    # is still parsed (the mini-repo fixture below uses it), so both token shapes are exercised
    # against a synthetic INPUT block rather than against whatever the real one happens to say.
    _, dirs = cdl._doxygen_input()
    check((R / "build/docs").resolve() in dirs, "Doxyfile INPUT staging root parsed")

    with tempfile.TemporaryDirectory() as td:
        dox = Path(td) / "Doxyfile"
        dox.write_text("INPUT = components/ \\\n        build/docs/README.md \\\n        build/docs/adr/\n")
        saved = cdl.DOXYFILE
        try:
            cdl.DOXYFILE = dox
            files, dirs = cdl._doxygen_input()
        finally:
            cdl.DOXYFILE = saved
    check((R / "build/docs/README.md").resolve() in files, "Doxyfile INPUT file token parsed")
    check((R / "build/docs/adr").resolve() in dirs, "Doxyfile INPUT directory token parsed")

    m = cdl._GITHUB_SELF_RE.match(
        "https://github.com/laberning/home_io_control/blob/main/config/x.yaml"
    )
    check(m and m.group(1) == "blob" and m.group(2) == "config/x.yaml", "self-blob URL parsed")


# ---------------------------------------------------------------------------
# check-docs-links.py — whole-repo checks against a throwaway repo
# ---------------------------------------------------------------------------

def _mini_repo(root: Path) -> None:
    """A minimal repo that mirrors the real doc wiring closely enough to exercise the checks."""
    (root / "docs" / "adr").mkdir(parents=True)
    (root / "config").mkdir()
    (root / "components" / "home_io_control").mkdir(parents=True)
    (root / "analysis").mkdir()

    # The README is the mainpage and so the root of the tree: check E requires every *other*
    # staged page to be named in exactly one subpages block, so it parents the two docs/ pages.
    (root / "README.md").write_text(
        "# R\n\n[guide](docs/guide.md)\n\n"
        "<!-- doxygen-subpages -->\n"
        "- [Guide](docs/guide.md)\n"
        "- [Arch](docs/arch.md)\n"
        "<!-- /doxygen-subpages -->\n"
    )
    (root / "docs" / "guide.md").write_text("# Guide\n<!-- doxygen-label: guide -->\n")
    (root / "docs" / "arch.md").write_text(
        "# Arch\n<!-- doxygen-label: arch -->\n\n"
        "<!-- doxygen-subpages -->\n- [ADRs](adr/README.md)\n<!-- /doxygen-subpages -->\n"
    )
    (root / "docs" / "adr" / "README.md").write_text(
        "# ADRs\n<!-- doxygen-label: adr_index -->\n\n"
        "<!-- doxygen-subpages -->\n"
        "- [ADR 0001](0001-a.md)\n"
        "<!-- /doxygen-subpages -->\n"
    )
    (root / "docs" / "adr" / "0001-a.md").write_text("# ADR 0001\n<!-- doxygen-label: adr0001 -->\n")
    (root / "config" / "board.yaml").write_text("x: 1\n")
    (root / "analysis" / "notes.md").write_text("secret notes\n")

    (root / "Doxyfile").write_text(
        "INPUT = components/home_io_control/ \\\n"
        "        build/docs/README.md \\\n"
        "        build/docs/guide.md \\\n"
        "        build/docs/arch.md \\\n"
        "        build/docs/adr/\n"
        "FILE_PATTERNS = *.h *.md\n"
    )
    (root / ".gitignore").write_text("analysis/\n")
    subprocess.run(["git", "init", "-q"], cwd=root, check=True)
    subprocess.run(["git", "add", "-A"], cwd=root, check=True)
    subprocess.run(
        ["git", "-c", "user.email=t@t", "-c", "user.name=t", "commit", "-qm", "x"],
        cwd=root, check=True,
    )


def _run_checks_in(root: Path) -> list[str]:
    # check-docs-links.py loaded its own private copy of stage-docs.py -- patch that one.
    sdx = cdl.stage_docs
    saved = {k: getattr(cdl, k) for k in ("REPO_ROOT", "DOXYFILE", "DOCS_DIR", "_DOXYGEN_SOURCE_DIRS")}
    saved_sd = {k: getattr(sdx, k) for k in ("REPO_ROOT", "DOCS_OUT_DIR", "SOURCE_GLOBS")}
    try:
        cdl.REPO_ROOT = root
        cdl.DOXYFILE = root / "Doxyfile"
        cdl.DOCS_DIR = root / "docs"
        cdl._DOXYGEN_SOURCE_DIRS = (root / "components" / "home_io_control",)
        cdl._SLUG_CACHE.clear()
        sdx.REPO_ROOT = root
        sdx.DOCS_OUT_DIR = root / "build" / "docs"
        sdx.SOURCE_GLOBS = ["README.md", "docs/*.md", "docs/adr/*.md"]
        errors: list[str] = []
        cdl.check_crosslinks(errors)
        cdl.check_published_docs(errors)
        return errors
    finally:
        for k, v in saved.items():
            setattr(cdl, k, v)
        for k, v in saved_sd.items():
            setattr(sdx, k, v)


def test_clean_mini_repo_passes() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        _mini_repo(root)
        errors = _run_checks_in(root)
        check(errors == [], f"clean mini-repo should pass, got: {errors}")


def test_each_violation_is_caught() -> None:
    cases = {
        "missing label": lambda r: (r / "docs" / "guide.md").write_text("# Guide\n\nno label\n"),
        "duplicate label": lambda r: (r / "docs" / "guide.md").write_text(
            "# Guide\n<!-- doxygen-label: arch -->\n"
        ),
        "relative link to non-INPUT file": lambda r: (r / "docs" / "guide.md").write_text(
            "# Guide\n<!-- doxygen-label: guide -->\n\n[c](../config/board.yaml)\n"
        ),
        "relative link to a directory": lambda r: (r / "docs" / "guide.md").write_text(
            "# Guide\n<!-- doxygen-label: guide -->\n\n[c](../config/)\n"
        ),
        "ADR dropped from subpages block": lambda r: (r / "docs" / "adr" / "0002-b.md").write_text(
            "# ADR 0002\n<!-- doxygen-label: adr0002 -->\n"
        ),
        "published doc links git-excluded": lambda r: (r / "docs" / "guide.md").write_text(
            "# Guide\n<!-- doxygen-label: guide -->\n\n[n](../analysis/notes.md)\n"
        ),
        "github blob link to missing path": lambda r: (r / "docs" / "guide.md").write_text(
            "# Guide\n<!-- doxygen-label: guide -->\n\n"
            "[x](https://github.com/laberning/home_io_control/blob/main/config/nope.yaml)\n"
        ),
        # check E, widened past ADRs: a staged page nobody subpages is unreachable in the tree.
        "non-ADR page dropped from subpages block": lambda r: (r / "README.md").write_text(
            "# R\n\n<!-- doxygen-subpages -->\n- [Arch](docs/arch.md)\n<!-- /doxygen-subpages -->\n"
        ),
        # check G: `## Notes` on two staged pages -> doxygen numbers the anchor site-wide.
        "cross-page link to a duplicated heading": lambda r: (
            (r / "docs" / "guide.md").write_text(
                "# Guide\n<!-- doxygen-label: guide -->\n\n## Notes\n\n[a](arch.md#notes)\n"
            ),
            (r / "docs" / "arch.md").write_text(
                "# Arch\n<!-- doxygen-label: arch -->\n\n## Notes\n\n"
                "<!-- doxygen-subpages -->\n- [ADRs](adr/README.md)\n<!-- /doxygen-subpages -->\n"
            ),
        ),
        # check G again: an in-page fragment is a global HTML id too, so it breaks the same way.
        "in-page link to a duplicated heading": lambda r: (
            (r / "docs" / "guide.md").write_text(
                "# Guide\n<!-- doxygen-label: guide -->\n\n## Notes\n\n[a](#notes)\n"
            ),
            (r / "docs" / "arch.md").write_text(
                "# Arch\n<!-- doxygen-label: arch -->\n\n## Notes\n\n"
                "<!-- doxygen-subpages -->\n- [ADRs](adr/README.md)\n<!-- /doxygen-subpages -->\n"
            ),
        ),
    }
    for name, mutate in cases.items():
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _mini_repo(root)
            mutate(root)
            subprocess.run(["git", "add", "-A"], cwd=root, check=True)
            errors = _run_checks_in(root)
            check(errors != [], f"violation not caught: {name}")


# ---------------------------------------------------------------------------

def main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    for t in tests:
        t()
    if _failures:
        print(f"check_docs_links_test: {len(_failures)} failure(s):", file=sys.stderr)
        for f in _failures:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print(f"check_docs_links_test: OK ({len(tests)} tests)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
