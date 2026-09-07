#!/usr/bin/env python3
"""Post-build checks over the generated doxygen site.

`scripts/check-docs-links.py` resolves links from the *source* `docs/` tree on the
developer's filesystem, where paths like `../components/home_io_control/hub_core.h`
exist -- so it is a GitHub-render checker, and it passes links that 404 on the
published Pages site. This script is the other half: it walks the *generated*
`docs/doxygen/html/` tree.

Two checks:

1. Broken links -- every relative `href` must resolve to a file that exists inside
   the output tree. A baseline file (`scripts/doxygen-link-baseline.txt`) lists
   links that are already broken and not yet fixed; the crawl fails when a broken
   link appears that is not in the baseline, or a baseline entry no longer matches
   anything (the baseline can only shrink).

   Baseline line forms (``#`` comments and blank lines ignored):

       page.html -> href        exact page + href pair
       * -> href                this href is baselined on any page

2. Markdown-stub autolinks -- doxygen 1.18 emits a stub "File Reference" page for
   every Markdown INPUT (`home__io__control_8md.html` and friends) and autolinks a
   bare filename to it, even inside inline code spans. Those stubs have empty
   bodies and leak `build/docs/...` in their titles, so no real page should link
   to one. When a new doc filename gets mentioned in prose, add it to
   `AUTOLINK_IGNORE_WORDS` in the Doxyfile.

Run as the last step of `scripts/generate-doxygen.sh`; not wired into `make lint`
because it needs the built HTML and `make check` already depends on `doxygen`.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from urllib.parse import unquote

REPO_ROOT = Path(__file__).parent.parent
HTML_ROOT = (REPO_ROOT / "docs" / "doxygen" / "html").resolve()
BASELINE_FILE = Path(__file__).parent / "doxygen-link-baseline.txt"

_HREF_RE = re.compile(r'href="([^"]+)"')
_HTML_COMMENT_RE = re.compile(r"<!--.*?-->", re.DOTALL)
_SKIP_PREFIXES = ("http://", "https://", "#", "mailto:", "javascript:", "data:")
# A generated file page for a Markdown input (and its source view).
_MD_STUB_RE = re.compile(r"_8md(_source)?\.html$")
# doxygen's own "Validator / crawler helper" -- links to every page by design; not
# a page a reader navigates to.
_EXCLUDE_PAGES = {"doxygen_crawl.html"}


def _pages() -> list[Path]:
    return sorted(p for p in HTML_ROOT.rglob("*.html") if p.name not in _EXCLUDE_PAGES)


def _clean(html_file: Path) -> str:
    # A link inside an HTML comment (e.g. the commented-out favicon <link> in
    # header.html) is never requested by a browser -- don't count it as broken.
    return _HTML_COMMENT_RE.sub("", html_file.read_text(encoding="utf-8", errors="replace"))


def _rel(page: Path) -> str:
    return str(page.relative_to(HTML_ROOT))


def _broken_links() -> set[str]:
    """Every ``page -> href`` in the generated tree whose target is missing."""
    broken: set[str] = set()
    for page in _pages():
        for href in _HREF_RE.findall(_clean(page)):
            if href.startswith(_SKIP_PREFIXES):
                continue
            target = unquote(href.split("#", 1)[0])
            if not target:
                continue
            resolved = (page.parent / target).resolve()
            inside = resolved == HTML_ROOT or HTML_ROOT in resolved.parents
            if inside and resolved.exists():
                continue
            broken.add(f"{_rel(page)} -> {href}")
    return broken


def _stub_autolinks() -> list[str]:
    """Real pages that link to a Markdown-stub file page (see check 2)."""
    hits: list[str] = []
    for page in _pages():
        if _MD_STUB_RE.search(page.name):
            continue
        for href in _HREF_RE.findall(_clean(page)):
            if _MD_STUB_RE.search(unquote(href.split("#", 1)[0])):
                hits.append(f"{_rel(page)} -> {href}")
    return sorted(set(hits))


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
        return entry.partition(" -> ")[2] in wildcard

    regressions = sorted(e for e in broken if not covered(e))
    broken_hrefs = {e.partition(" -> ")[2] for e in broken}
    stale_exact = sorted(exact - broken)
    stale_wildcard = sorted(w for w in wildcard if w not in broken_hrefs)
    stub_links = _stub_autolinks()

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
    if stub_links:
        print(
            f"{len(stub_links)} link(s) to a Markdown-stub file page (empty body, leaks "
            "build/docs/ in the title) -- add the filename to AUTOLINK_IGNORE_WORDS in Doxyfile:",
            file=sys.stderr,
        )
        for entry in stub_links:
            print(f"  - {entry}", file=sys.stderr)

    if regressions or stale_exact or stale_wildcard or stub_links:
        return 1

    baselined = len(exact) + len(wildcard)
    note = f" ({baselined} known-broken baselined)" if baselined else ""
    print(f"Generated-docs link crawl OK{note}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
