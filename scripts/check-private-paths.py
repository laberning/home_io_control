#!/usr/bin/env python3
"""Fail any tracked file that names a local-only path.

Some paths exist only in a developer's own checkout and are never published. A tracked file that
points there -- a code comment, a corpus capture's provenance note, a test fixture's header, a
doc -- points at nothing for every other reader. Restate the fact and cite repository evidence
instead (a corpus capture id, an ADR, a GitHub issue); an outside project is named by its public
name and URL.

A hit is one of the names in PRIVATE_PATH_RE used as a path from the repository root: not
preceded by a word character, `.`, `/` or `-`, unless what precedes it is a relative-path step
(`./`, `../`) or the repository's own directory name. Plain words and longer paths that merely
contain such a name pass. The checker scripts are exempt as whole files; any other legitimate
mention takes a narrow "<path>:<substring>" entry in ALLOWLIST.

Run via `make private-path-check`; part of the `lint` composite target.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

PRIVATE_PATH_RE = re.compile(
    r"(?:(?<![\w./-])|(?<=\./)|(?<=home_io_control/))"
    r"(?:(?:analysis|lab)/|reference/[A-Za-z]|AGENTS\.md)")

# The checker scripts and their self-tests must name the paths to apply or exercise the rule.
EXEMPT_FILES = frozenset({
    "scripts/check-private-paths.py",
    "scripts/check_private_paths_test.py",
    "scripts/check-docs-prose.py",
    "scripts/check-docs-links.py",
    "scripts/check_docs_links_test.py",
    "scripts/check-key-material.py",
})

# "<repo-relative path>:<substring that must appear on the line>".
ALLOWLIST = frozenset({
})


def find_hits(rel_path: str, text: str) -> list[tuple[int, str]]:
    """Return (line number, line) for every line of `text` that names a private path."""
    if rel_path in EXEMPT_FILES:
        return []
    prefix = rel_path + ":"
    allowed = [entry[len(prefix):] for entry in ALLOWLIST if entry.startswith(prefix)]
    hits = []
    for n, line in enumerate(text.split("\n"), 1):
        if PRIVATE_PATH_RE.search(line) and not any(sub in line for sub in allowed):
            hits.append((n, line))
    return hits


def tracked_text_files() -> list[tuple[str, str]]:
    """(repo-relative path, text) for every tracked file that decodes as UTF-8."""
    listed = subprocess.run(["git", "ls-files", "-z"], cwd=REPO_ROOT, check=True,
                            capture_output=True).stdout.decode("utf-8").split("\0")
    files = []
    for rel in filter(None, listed):
        path = REPO_ROOT / rel
        if not path.is_file():  # deleted in the working tree, or a symlink to a folder
            continue
        try:
            files.append((rel, path.read_text(encoding="utf-8")))
        except UnicodeDecodeError:  # binary: images, fonts, firmware blobs
            continue
    return files


def main() -> int:
    errors = []
    files = tracked_text_files()
    for rel, text in files:
        for n, line in find_hits(rel, text):
            errors.append(f"{rel}:{n}: {line.strip()[:110]}")
    if errors:
        print("Private-path check failed: these tracked lines name a local-only path, which a reader "
              "cannot open. Restate the fact and cite repository evidence (a corpus capture id, an "
              "ADR, a GitHub issue) instead:", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1
    print(f"Private paths OK ({len(files)} tracked text file(s) checked).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
