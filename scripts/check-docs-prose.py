#!/usr/bin/env python3
"""Prose gate for the published docs' two unenforceable-looking writing rules.

`check-docs-links.py` resolves links; this checks the prose around them. Two
independent pattern sets, deliberately small and high-precision -- a gate that
cries wolf gets disabled, and these rules are mostly upheld by review.

1. **Provenance.** A published page must never name git-excluded working notes as
   the source of a fact: they are deleted or never shipped, so a reader cannot
   open them. Check C in check-docs-links.py already fails a *link* into that
   territory -- this catches the far commoner shape, a backticked path in prose,
   which no link checker sees. Matched outside fenced blocks, so an example may
   still name a local path.

   The vendor configuration-tool material is the strict half of this rule: it
   must not appear in the repository in any form, so it is matched against the
   raw file, fences included.

   Unlike the history rule below, neither has a `docs/adr/` exemption: an ADR is
   a published page and its reader is in exactly the same position.

2. **History.** The docs describe what is true now, so "used to be", "no longer"
   and friends are usually a sentence that should have been deleted or rewritten
   in the present tense. `docs/adr/` *is* exempt: a decision record is about
   change by design, and the Considered/Rejected/Superseded structure depends on
   it.

Both sets take an inline allowlist, keyed by "path:substring" -- narrow on
purpose, so adding an entry is a deliberate act rather than a wildcard.

Run via `make docs-prose-check`; part of the `lint` composite target.
"""

from __future__ import annotations

import importlib.util
import re
import sys
from pathlib import Path

from md_fences import strip_fenced_blocks

REPO_ROOT = Path(__file__).parent.parent

# Which files ship is stage-docs.py's business (hyphen in the name -> load by path).
_spec = importlib.util.spec_from_file_location("stage_docs", Path(__file__).parent / "stage-docs.py")
stage_docs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(stage_docs)

# --- rule 1: provenance -----------------------------------------------------
# A path that identifies git-excluded working notes. `reference/` and `analysis/`
# need the trailing slash: bare "reference" and "analysis" are ordinary English
# words that appear constantly in these docs. Checked outside fenced blocks --
# a shell command or a YAML example may legitimately name a local path.
PROVENANCE_RE = re.compile(r"analysis/|reference/|AGENTS\.md", re.I)

# The vendor-tool material is the strict half: it may not appear in this repo in
# any form, so unlike the paths above it is matched against the *raw* file,
# fenced blocks included. A code example is not an exemption from that rule.
VENDOR_DATA_RE = re.compile(r"setgo|set&go|set_go", re.I)

# --- rule 2: history --------------------------------------------------------
HISTORY_RE = re.compile(
    r"used to be|no longer|in a previous version|formerly|renamed from|"
    r"deprecated in favou?r of|this page was",
    re.I,
)

# "<repo-relative path>:<substring that must appear on the line>".
ALLOWLIST = {
    # Describes the directory's role in the workflow; sources no fact from it.
    "docs/adr/0034-doxygen-syntax-generated-at-staging-time.md:"
    "planning documents that drove this work will be deleted",
    # Domain usage: the device's key no longer matching, not this repo's history.
    "docs/troubleshooting.md:The identity's `system_key` no longer matches",
    # The style guide quotes the banned phrasing in order to ban it.
    "docs/contributing.md:not \"lights are no longer experimental\" but",
}


def _rel(p: Path) -> str:
    return str(p.relative_to(REPO_ROOT))


def _allowed(path: Path, line: str) -> bool:
    prefix = _rel(path) + ":"
    return any(
        entry.startswith(prefix) and entry[len(prefix):] in line for entry in ALLOWLIST
    )


def main() -> int:
    errors: list[str] = []
    checked = 0

    for src in stage_docs._sources():
        checked += 1
        is_adr = src.parent.name == "adr"
        raw = src.read_text(encoding="utf-8")
        for n, line in enumerate(raw.split("\n"), 1):
            if not _allowed(src, line) and VENDOR_DATA_RE.search(line):
                errors.append(
                    f"{_rel(src)}:{n}: names the vendor configuration-tool material, which must "
                    f"not appear in this repository in any form -- not cited, quoted, paraphrased, "
                    f"or alluded to.\n      {line.strip()[:110]}"
                )
        # Fenced blocks are examples, not prose -- a YAML snippet or a shell
        # command may legitimately mention any of the paths below.
        text = strip_fenced_blocks(raw)
        for n, line in enumerate(text.split("\n"), 1):
            if _allowed(src, line):
                continue
            if PROVENANCE_RE.search(line):
                errors.append(
                    f"{_rel(src)}:{n}: names git-excluded working notes as a source -- a reader "
                    f"cannot open them. Restate the fact and cite repo evidence (a corpus capture "
                    f"id, an ADR, a GitHub issue) instead.\n      {line.strip()[:110]}"
                )
            # A decision record is about change by design; the history rule does not apply.
            if not is_adr and HISTORY_RE.search(line):
                errors.append(
                    f"{_rel(src)}:{n}: narrates history -- the docs describe what is true now. "
                    f"Rewrite in the present tense, or delete the sentence.\n"
                    f"      {line.strip()[:110]}"
                )

    if errors:
        print("Documentation prose check failed:", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        print(
            "\nIf a hit is legitimate domain usage, add a narrow "
            "'<path>:<substring>' entry to ALLOWLIST in this script.",
            file=sys.stderr,
        )
        return 1

    print(f"Documentation prose OK ({checked} published file(s) checked).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
