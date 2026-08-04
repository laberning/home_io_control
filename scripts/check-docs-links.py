#!/usr/bin/env python3
"""Documentation cross-link check.

Every relative markdown link (a file path, or an in-page `#fragment`) across
the *tracked* documentation set must resolve to something real -- a doc
renamed or a heading reworded without updating what links to it is a silent
break otherwise. External (http/https/mailto) links are not checked here;
nothing in this repo can assert those stay live.

Scoped to git-tracked files only, in both directions: only tracked markdown
files are scanned as link sources, and a link whose target is gitignored
(analysis/, reference/, AGENTS.md, ...) is skipped rather than flagged --
those are private working files that never ship with the repo, not part of
the doc set this check owns.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).parent.parent
LINK_RE = re.compile(r"\[[^\]]*\]\(([^)]+)\)")

# Matches a fenced code block (``` or ~~~, three or more of either character) through to its
# closing fence, so example YAML/bash containing '#' comments or markdown-looking links doesn't
# get scanned as real headings/links. Mirrors the same closing-fence-reuses-the-opening-marker
# assumption scripts/pygmentize-md-yaml.py's _FENCE_RE makes -- good enough for this repo's docs,
# not a full CommonMark fence parser.
_FENCE_STRIP_RE = re.compile(r"^(```+|~~~+)[^\n]*\n.*?^\1[^\n]*$", re.DOTALL | re.MULTILINE)

# Relative-looking links that actually point at GitHub's own UI (a repo's
# releases/issues page), not a file tracked in this repository.
_GITHUB_UI_ALLOWLIST = {"../../releases", "../../issues", "../../pulls", "../../actions"}

# Per-file heading slugs, keyed by resolved path. A doc set with many cross-links can otherwise
# re-read and re-parse the same target file once per incoming link.
_SLUG_CACHE: dict[Path, set[str]] = {}


def _tracked_markdown_files() -> list[Path]:
    output = subprocess.run(
        ["git", "ls-files", "*.md"],
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return sorted(REPO_ROOT / line for line in output.splitlines() if line)


def _is_gitignored(path: Path) -> bool:
    result = subprocess.run(
        ["git", "check-ignore", "-q", str(path)],
        cwd=REPO_ROOT,
        check=False,
    )
    return result.returncode == 0


def _heading_slugs(text: str) -> set[str]:
    """GitHub's heading-anchor algorithm: lowercase, spaces to hyphens,
    everything but word characters/hyphens/spaces stripped.
    """
    slugs = set()
    for line in text.splitlines():
        if line.startswith("#"):
            heading = line.lstrip("#").strip()
            slug = re.sub(r"[^\w\s-]", "", heading).strip().lower().replace(" ", "-")
            slugs.add(slug)
    return slugs


def _slugs_for(path: Path) -> set[str]:
    """Heading slugs for a tracked markdown file, cached across the whole run."""
    if path not in _SLUG_CACHE:
        _SLUG_CACHE[path] = _heading_slugs(_FENCE_STRIP_RE.sub("", path.read_text()))
    return _SLUG_CACHE[path]


def main() -> int:
    errors: list[str] = []
    doc_files = _tracked_markdown_files()
    tracked = set(doc_files)

    for md_file in doc_files:
        text = _FENCE_STRIP_RE.sub("", md_file.read_text())
        slugs = _slugs_for(md_file)
        for link in LINK_RE.findall(text):
            if link.startswith(("http://", "https://", "mailto:")):
                continue
            if link in _GITHUB_UI_ALLOWLIST:
                continue

            target, _, fragment = link.partition("#")
            if target:
                resolved = (md_file.parent / target).resolve()
                if not resolved.exists():
                    if not _is_gitignored(resolved):
                        errors.append(f"{md_file.relative_to(REPO_ROOT)}: broken link to '{target}'")
                    continue
                # Cross-file anchor: only checkable when the target is itself a tracked markdown
                # file whose headings we can compute. Non-markdown targets (images, configs, ...)
                # and gitignored targets keep the existence-only check above.
                if fragment and resolved.suffix == ".md" and resolved in tracked:
                    if fragment not in _slugs_for(resolved):
                        errors.append(
                            f"{md_file.relative_to(REPO_ROOT)}: broken anchor '#{fragment}' "
                            f"in link to '{target}'"
                        )
            elif fragment and fragment not in slugs:
                errors.append(
                    f"{md_file.relative_to(REPO_ROOT)}: broken in-page link to '#{fragment}'"
                )

    if errors:
        print("Documentation link check failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    print(f"Documentation links OK ({len(doc_files)} file(s) checked).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
