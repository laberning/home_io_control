#!/usr/bin/env python3
"""Documentation link + staging-invariant check (fast, no doxygen build).

Two groups of checks:

1. Cross-links (whole tracked markdown set). Every relative link -- a file path
   or an in-page `#fragment` -- must resolve. A doc renamed or a heading reworded
   without updating what points at it is a silent break otherwise. http(s)/mailto
   links are not checked; nothing here can assert those stay live. A link to a
   gitignored target (analysis/, reference/, AGENTS.md) is normally skipped --
   those are private working notes -- EXCEPT from a published doc, where it would
   ship a dead link (check C below).

2. Published-doc / doxygen-staging invariants (README + docs/**). The generated
   site is built from `scripts/stage-docs.py` + `Doxyfile` INPUT; these checks
   keep the two honest so a new or moved doc cannot silently fall off the site,
   collide on a page label, or drop out of the ADR tree. Anything that needs the
   staging rules (label regex, subpage regex, source globs, dest mapping) imports
   them from stage-docs.py rather than re-encoding them.

The post-build crawl (scripts/check-doxygen-output-links.py, run by
generate-doxygen.sh) is the belt to this braces -- it sees the real HTML.
"""

from __future__ import annotations

import importlib.util
import re
import subprocess
import sys
from pathlib import Path

from md_fences import strip_fenced_blocks

REPO_ROOT = Path(__file__).parent.parent
DOXYFILE = REPO_ROOT / "Doxyfile"
DOCS_DIR = REPO_ROOT / "docs"

LINK_RE = re.compile(r"\[[^\]]*\]\(([^)]+)\)")

# Relative-looking links that actually point at GitHub's own UI (a repo's
# releases/issues page), not a file tracked in this repository.
_GITHUB_UI_ALLOWLIST = {"../../releases", "../../issues", "../../pulls", "../../actions"}

# An absolute link back into this same repo on github.com. The path is checkable:
# blob/<ref>/<p> -> <p> must be a tracked file; tree/<ref>/<p> -> a tracked directory.
# (Used where a relative link cannot resolve on the Pages site, e.g. config/**.)
_GITHUB_SELF_RE = re.compile(
    r"https://github\.com/laberning/home_io_control/(blob|tree)/[^/]+/([^)\s#?]+)"
)

# Per-file heading slugs, keyed by resolved path.
_SLUG_CACHE: dict[Path, set[str]] = {}

# stage-docs.py is not importable by name (hyphen); load it by path so the label
# regex, subpage regex, source globs and dest mapping have a single definition.
_spec = importlib.util.spec_from_file_location("stage_docs", Path(__file__).parent / "stage-docs.py")
stage_docs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(stage_docs)

# doxygen INPUT source dirs whose files doxygen resolves a relative link to
# without staging (the C++ tree). Used by check D.
_DOXYGEN_SOURCE_DIRS = (REPO_ROOT / "components" / "home_io_control",)


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def _rel(p: Path) -> str:
    try:
        return str(p.relative_to(REPO_ROOT))
    except ValueError:
        return str(p)


def _tracked_markdown_files() -> list[Path]:
    out = subprocess.run(
        ["git", "ls-files", "*.md"],
        cwd=REPO_ROOT, check=True, capture_output=True, text=True,
    ).stdout
    return sorted(REPO_ROOT / line for line in out.splitlines() if line)


def _tracked_paths() -> set[str]:
    """Every git-tracked path, repo-root-relative (POSIX)."""
    out = subprocess.run(
        ["git", "ls-files"], cwd=REPO_ROOT, check=True, capture_output=True, text=True,
    ).stdout
    return {line for line in out.splitlines() if line}


def _is_gitignored(path: Path) -> bool:
    return subprocess.run(
        ["git", "check-ignore", "-q", str(path)], cwd=REPO_ROOT, check=False
    ).returncode == 0


def _is_published_path(md: Path) -> bool:
    """A doc that ships to the generated site: README, or anything under docs/."""
    if md == REPO_ROOT / "README.md":
        return True
    return DOCS_DIR in md.parents


def _heading_slugs(text: str) -> set[str]:
    slugs = set()
    for line in text.splitlines():
        if line.startswith("#"):
            heading = line.lstrip("#").strip()
            slugs.add(re.sub(r"[^\w\s-]", "", heading).strip().lower().replace(" ", "-"))
    return slugs


def _slugs_for(path: Path) -> set[str]:
    if path not in _SLUG_CACHE:
        _SLUG_CACHE[path] = _heading_slugs(strip_fenced_blocks(path.read_text()))
    return _SLUG_CACHE[path]


def _labels_in(path: Path) -> set[str]:
    return set(stage_docs._LABEL_RE.findall(path.read_text(encoding="utf-8")))


def _doxygen_input() -> tuple[set[Path], set[Path]]:
    """(explicit files, directories) named in the Doxyfile INPUT setting, resolved."""
    files: set[Path] = set()
    dirs: set[Path] = set()
    text = DOXYFILE.read_text()
    m = re.search(r"^INPUT\s*=\s*((?:[^\n]*\\\n)*[^\n]*)", text, re.M)
    if not m:
        return files, dirs
    for tok in m.group(1).replace("\\\n", " ").split():
        p = (REPO_ROOT / tok).resolve()
        (dirs if tok.endswith("/") else files).add(p)
    return files, dirs


def _reachable_by_doxygen(staged: Path, in_files: set[Path], in_dirs: set[Path]) -> bool:
    staged = staged.resolve()
    return staged in in_files or any(d in staged.parents for d in in_dirs)


# ---------------------------------------------------------------------------
# check group 1: cross-links
# ---------------------------------------------------------------------------

def check_crosslinks(errors: list[str]) -> int:
    doc_files = _tracked_markdown_files()
    tracked = set(doc_files)
    tracked_paths = _tracked_paths()

    for md_file in doc_files:
        published = _is_published_path(md_file)
        text = strip_fenced_blocks(md_file.read_text())
        slugs = _slugs_for(md_file)
        for link in LINK_RE.findall(text):
            gh = _GITHUB_SELF_RE.match(link)
            if gh:
                kind, path = gh.group(1), gh.group(2).rstrip("/")
                if kind == "blob" and path not in tracked_paths:
                    errors.append(f"{_rel(md_file)}: github blob link to '{path}', not a tracked file")
                elif kind == "tree" and not any(
                    p == path or p.startswith(path + "/") for p in tracked_paths
                ):
                    errors.append(f"{_rel(md_file)}: github tree link to '{path}', not a tracked directory")
                continue
            if link.startswith(("http://", "https://", "mailto:")):
                continue
            if link in _GITHUB_UI_ALLOWLIST:
                continue

            target, _, fragment = link.partition("#")
            if target:
                resolved = (md_file.parent / target).resolve()
                if not resolved.exists():
                    if not _is_gitignored(resolved):
                        errors.append(f"{_rel(md_file)}: broken link to '{target}'")
                    continue
                # check C: a shipped doc must not link into git-excluded territory.
                if published and _is_gitignored(resolved):
                    errors.append(
                        f"{_rel(md_file)}: links to '{target}', which is git-excluded and "
                        f"never published -- use an absolute URL or remove it"
                    )
                    continue
                if fragment and resolved.suffix == ".md" and resolved in tracked:
                    if fragment not in _slugs_for(resolved):
                        errors.append(
                            f"{_rel(md_file)}: broken anchor '#{fragment}' in link to '{target}'"
                        )
            elif fragment and fragment not in slugs:
                errors.append(f"{_rel(md_file)}: broken in-page link to '#{fragment}'")

    return len(doc_files)


# ---------------------------------------------------------------------------
# check group 2: published-doc / staging invariants
# ---------------------------------------------------------------------------

def check_published_docs(errors: list[str]) -> None:
    sources = stage_docs._sources()                       # README + docs/*.md + docs/adr/*.md
    src_set = set(sources)
    in_files, in_dirs = _doxygen_input()
    readme = REPO_ROOT / "README.md"

    # --- A. reachability, both directions -----------------------------------
    for md in _tracked_markdown_files():
        if _is_published_path(md) and md not in src_set:
            errors.append(
                f"{_rel(md)}: tracked under docs/ but stage-docs.py does not stage it "
                f"(add its glob to SOURCE_GLOBS) -- it would be invisible on the site"
            )
    for src in sources:
        dest = stage_docs._dest_path(src)
        if not _reachable_by_doxygen(dest, in_files, in_dirs):
            errors.append(
                f"{_rel(src)}: staged to {_rel(dest)}, which no Doxyfile INPUT entry covers "
                f"-- add it to INPUT"
            )

    # --- B. label presence + uniqueness ----------------------------------------
    label_owner: dict[str, Path] = {}
    for src in sources:
        found = list(stage_docs._LABEL_RE.findall(src.read_text(encoding="utf-8")))
        if src != readme and not found:
            errors.append(
                f"{_rel(src)}: no <!-- doxygen-label: NAME --> comment "
                f"(a staged page needs one for a stable URL and to nest in the tree)"
            )
        if len(set(found)) > 1:
            errors.append(f"{_rel(src)}: declares more than one doxygen-label: {sorted(set(found))}")
        for lbl in set(found):
            if lbl in label_owner:
                errors.append(
                    f"doxygen-label '{lbl}' is used by both {_rel(label_owner[lbl])} and "
                    f"{_rel(src)} -- labels must be unique (a collision silently merges pages)"
                )
            else:
                label_owner[lbl] = src

    # --- D. every shipped relative link lands somewhere doxygen emits ---------
    for src in sources:
        text = strip_fenced_blocks(src.read_text(encoding="utf-8"))
        for link in LINK_RE.findall(text):
            if link.startswith(("http://", "https://", "mailto:")) or link.startswith("#"):
                continue
            if link in _GITHUB_UI_ALLOWLIST:
                continue
            target = link.partition("#")[0]
            if not target:
                continue
            resolved = (src.parent / target).resolve()
            if not resolved.exists():
                continue  # existence handled by check_crosslinks
            if _is_gitignored(resolved):
                continue  # check C owns "shipped doc links git-excluded target"
            in_cpp_tree = any(d == resolved or d in resolved.parents for d in _DOXYGEN_SOURCE_DIRS)
            ok = (
                resolved in src_set                                   # another published doc
                or in_cpp_tree                                        # C++ tree, doxygen resolves it
                or stage_docs._BOUNDARY_LINK_RE.search(f"]({link})")  # README docs/ -> build/docs/ rewrite
            )
            if not ok:
                what = "directory" if resolved.is_dir() else "file"
                errors.append(
                    f"{_rel(src)}: relative link to {what} '{target}' resolves for GitHub but "
                    f"not on the generated site (not in doxygen INPUT, no staging rule) -- use "
                    f"an absolute URL"
                )

    # --- E. subpage-block integrity + every ADR parented once ----------------
    adr_files = sorted(
        p for p in sources
        if p.parent.name == "adr" and re.match(r"\d{4}-", p.name)
    )
    subpaged: dict[str, Path] = {}   # child label -> index file that lists it
    for src in sources:
        blocks = stage_docs._SUBPAGES_BLOCK_RE.findall(src.read_text(encoding="utf-8"))
        for block in blocks:
            for line in block.splitlines():
                m = stage_docs._BULLET_RE.match(line)
                if not m:
                    if line.strip():
                        errors.append(
                            f"{_rel(src)}: non-bullet line in a <!-- doxygen-subpages --> block: "
                            f"{line.strip()!r}"
                        )
                    continue
                child = (src.parent / m.group(1)).resolve()
                if not child.is_file():
                    errors.append(f"{_rel(src)}: subpage bullet targets missing file '{m.group(1)}'")
                    continue
                child_labels = _labels_in(child)
                if not child_labels:
                    errors.append(
                        f"{_rel(src)}: subpages '{m.group(1)}', which has no doxygen-label"
                    )
                    continue
                lbl = sorted(child_labels)[0]  # check B errors on >1; sorted keeps the report stable
                if lbl in subpaged:
                    errors.append(
                        f"'{m.group(1)}' (label '{lbl}') is subpaged by both "
                        f"{_rel(subpaged[lbl])} and {_rel(src)} -- a page can have one parent"
                    )
                else:
                    subpaged[lbl] = src
    for adr in adr_files:
        if not (_labels_in(adr) & subpaged.keys()):
            errors.append(
                f"{_rel(adr)}: not listed in any <!-- doxygen-subpages --> block "
                f"-- it will not appear in the sidebar tree"
            )

    # --- F. mermaid reachability -------------------------------------------
    for md in _tracked_markdown_files():
        if not _is_published_path(md):
            continue
        if "```mermaid" in md.read_text(encoding="utf-8") and md not in src_set:
            errors.append(
                f"{_rel(md)}: has a ```mermaid fence but is not staged -- the diagram will not render"
            )


# ---------------------------------------------------------------------------

def main() -> int:
    errors: list[str] = []
    n = check_crosslinks(errors)
    check_published_docs(errors)

    if errors:
        print("Documentation check failed:", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1

    print(f"Documentation links + staging invariants OK ({n} file(s) checked).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
