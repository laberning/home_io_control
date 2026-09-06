#!/usr/bin/env python3
"""Post-build link crawl over the generated doxygen site.

`scripts/check-docs-links.py` resolves links from the *source* `docs/` tree on the
developer's filesystem, where paths like `../components/home_io_control/hub_core.h`
exist -- so it is a GitHub-render checker, and it passes links that 404 on the
published Pages site. This script is the other half: it walks every `href` in the
*generated* `docs/doxygen/html/` tree and fails on any that points nowhere.

A baseline file (`scripts/doxygen-link-baseline.txt`) lists the links that are
already broken and not yet fixed. The crawl fails when:

  * a broken link appears that is *not* in the baseline (a regression), or
  * a baseline entry no longer matches any broken link (it was fixed, so its line
    must be removed -- the baseline can only shrink).

Baseline line forms (``#`` comments and blank lines ignored):

    page.html -> href        exact page + href pair
    * -> href                this href is baselined on any page (for chrome links
                             repeated across the whole tree)

Run as the last step of `scripts/generate-doxygen.sh`; not wired into `make lint`
because it needs the built HTML and `make check` already depends on `doxygen`.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from urllib.parse import unquote

REPO_ROOT = Path(__file__).parent.parent
HTML_ROOT = REPO_ROOT / "docs" / "doxygen" / "html"
BASELINE_FILE = Path(__file__).parent / "doxygen-link-baseline.txt"

_HREF_RE = re.compile(r'href="([^"]+)"')
_HTML_COMMENT_RE = re.compile(r"<!--.*?-->", re.DOTALL)
_SKIP_PREFIXES = ("http://", "https://", "#", "mailto:", "javascript:", "data:")


def _broken_links() -> set[str]:
    """Every ``page.html -> href`` in the generated tree whose target is missing."""
    broken: set[str] = set()
    for html_file in sorted(HTML_ROOT.glob("*.html")):
        text = html_file.read_text(encoding="utf-8", errors="replace")
        # A link inside an HTML comment (e.g. the commented-out favicon <link> in header.html)
        # is never requested by a browser -- don't count it as broken.
        text = _HTML_COMMENT_RE.sub("", text)
        for href in _HREF_RE.findall(text):
            if href.startswith(_SKIP_PREFIXES):
                continue
            target = unquote(href.split("#", 1)[0])
            if not target:
                continue
            if (HTML_ROOT / target).exists():
                continue
            broken.add(f"{html_file.name} -> {href}")
    return broken


def _load_baseline() -> tuple[set[str], set[str]]:
    """(exact ``page -> href`` entries, wildcard ``href`` entries)."""
    exact: set[str] = set()
    wildcard: set[str] = set()
    if not BASELINE_FILE.exists():
        return exact, wildcard
    for raw in BASELINE_FILE.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        page, sep, href = line.partition(" -> ")
        if not sep:
            print(f"malformed baseline line: {raw!r}", file=sys.stderr)
            sys.exit(2)
        if page.strip() == "*":
            wildcard.add(href.strip())
        else:
            exact.add(line)
    return exact, wildcard


def main() -> int:
    if not HTML_ROOT.is_dir():
        print(f"no generated docs at {HTML_ROOT} -- run `make doxygen` first", file=sys.stderr)
        return 2

    broken = _broken_links()
    exact, wildcard = _load_baseline()

    def covered(entry: str) -> bool:
        if entry in exact:
            return True
        _, _, href = entry.partition(" -> ")
        return href in wildcard

    regressions = sorted(e for e in broken if not covered(e))
    broken_hrefs = {e.partition(" -> ")[2] for e in broken}
    stale_exact = sorted(exact - broken)
    stale_wildcard = sorted(w for w in wildcard if w not in broken_hrefs)

    if regressions:
        print(f"{len(regressions)} new broken link(s) in the generated docs:", file=sys.stderr)
        for entry in regressions:
            print(f"  - {entry}", file=sys.stderr)
    if stale_exact or stale_wildcard:
        print(
            "baseline entries that are no longer broken -- remove them from "
            f"{BASELINE_FILE.relative_to(REPO_ROOT)}:",
            file=sys.stderr,
        )
        for entry in stale_exact:
            print(f"  - {entry}", file=sys.stderr)
        for href in stale_wildcard:
            print(f"  - * -> {href}", file=sys.stderr)

    if regressions or stale_exact or stale_wildcard:
        return 1

    baselined = len(exact) + len(wildcard)
    note = f" ({baselined} known-broken baselined)" if baselined else ""
    print(f"Generated-docs link crawl OK{note}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
