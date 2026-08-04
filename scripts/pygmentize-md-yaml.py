#!/usr/bin/env python3
# Pre-process Markdown files for Doxygen: convert fenced ```yaml / ```yml blocks
# into Pygments HTML so Doxygen renders them with syntax highlighting.
# Replacement preserves the original fence block's line count so Doxygen
# source-line references and warning line numbers remain correct.

import re
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR   = Path(__file__).parent.resolve()
REPO_ROOT    = SCRIPT_DIR.parent
DOXYFILE     = SCRIPT_DIR / "Doxyfile"
DOCS_OUT_DIR = REPO_ROOT / "build" / "docs"

# Non-recursive on purpose: docs/adr/ is a real directory under docs/ that Doxygen's INPUT never
# references (its ADRs aren't wired into the generated site — see docs/architecture_overview.md's
# "Design Decisions" section, which links to it on GitHub instead). A recursive glob would copy
# and pygmentize all of it here for nothing every run. Add a page explicitly below if it should
# ever actually become part of the generated docs.
EXTRA_MD_GLOBS = [
    "README.md",
    "docs/*.md",
]

# Matches fenced yaml blocks:
#   ```yaml
#   content lines (lazy)
#   ```
# The closing fence re-uses the exact backtick count from group(1).
_FENCE_RE = re.compile(
    r"^(```+)\s*(yaml|yml)\s*\n(.+?)\n\1",
    re.DOTALL | re.MULTILINE,
)


# ---------------------------------------------------------------------------
# Pygments
# ---------------------------------------------------------------------------

def _pygmentize(content: str) -> str:
    r = subprocess.run(
        ["pygmentize", "-f", "html", "-l", "yaml"],
        input=content,
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        print(f"pygmentize error: {r.stderr.strip()}", file=sys.stderr)
        sys.exit(1)
    return r.stdout.rstrip("\n")


# ---------------------------------------------------------------------------
# Line-count-safe replacement
# ---------------------------------------------------------------------------

def _replace_block(m: re.Match) -> str:
    """Replace one matched yaml fence with HTML that occupies the same number
    of file lines as the original fence block, preserving Doxygen line refs."""
    yaml_content = m.group(3)
    html = _pygmentize(yaml_content)

    # Rename the Pygments wrapper div from "highlight" to "fragment" so it
    # matches the CSS class used by the doxygen-awesome theme:
    #   • background / padding / border-radius come from div.fragment rules
    #   • doxygen-awesome-fragment-copy-button.js attaches copy buttons
    # Remove the inline "background: #f8f8f8" style that Pygments injects — it is
    # hardcoded for light mode and would override the doxygen-awesome theme's
    # dark-mode-aware CSS variable (--fragment-background).  A plain div.fragment
    # without an inline background: style correctly inherits either the light or
    # dark value depending on the active colour scheme.
    m_div = re.match(r'<div class="highlight"([^>]*)>', html)
    if m_div:
        attrs = m_div.group(1)
        # Strip any inline background declaration so CSS variables can apply.
        attrs = re.sub(r'\s*style\s*=\s*"[^"]*background[^"]*"', '', attrs)
        html = f'<div class="fragment"{attrs}>' + html[m_div.end():]

    # File-lines occupied by the original match (bounded by surrounding \n).
    # Each line in the file is counted by counting \n; for m = chars[55..58]
    # (0-based), the block spans file lines 55..58 → 4 lines:
    #   start_line = content[:m.start()].count('\n') + 1
    #   end_line   = content[:m.end()].count('\n') + 1
    #   file_lines = end_line - start_line + 1
    # Equivalently, count \n in the match text + 1 (since a 0-\n match = 1 line):
    orig_file_lines = m.group(0).count("\n") + 1

    n_html_lines = html.count("\n") + 1

    pad = max(0, orig_file_lines - n_html_lines)
    if pad == 0:
        return html

    # Pad with invisible <div class="line"><span></span></div> lines so the total
    # output matches the original block height in the file. The doxygen-awesome-css
    # theme renders <div class="line"> as a 1-line tall fragment, preserving blank-line
    # separation between the code block and any following prose.
    padding = "\n".join(["<div class=\"line\"><span></span></div>"] * pad)
    return f"{html}\n{padding}"


def transform(content: str) -> str:
    """Replace all yaml fenced blocks in *content* with highlighted HTML."""
    return _FENCE_RE.sub(_replace_block, content)


# ---------------------------------------------------------------------------
# File discovery
# ---------------------------------------------------------------------------

def _md_from_doxyfile() -> list[Path]:
    files: list[Path] = []
    if not DOXYFILE.exists():
        return files
    with open(DOXYFILE) as f:
        for line in f:
            line = line.strip()
            if line.startswith("#") or "=" not in line:
                continue
            key, _, val = line.partition("=")
            if key.strip() != "INPUT":
                continue
            for entry in val.strip().split():
                p = REPO_ROOT / entry
                if p.is_file() and p.suffix == ".md":
                    files.append(p)
                elif p.is_dir():
                    files.extend(p.rglob("*.md"))
    return files


def _all_sources() -> list[Path]:
    """Doxyfile INPUT files first, then everything else.

    The order is load-bearing: output names are flat basenames, so if two sources ever
    shared a basename they would compete for one destination. Files named in Doxyfile
    INPUT are referenced by that basename and must win — see _dest_name(), which relies
    on this ordering to decide who gets the plain name. Sorting the combined list would
    destroy that grouping, so each group is sorted separately instead."""
    seen: set[Path] = set()
    out: list[Path] = []
    for f in sorted(_md_from_doxyfile()):
        if f not in seen:
            seen.add(f); out.append(f)  # noqa: E701
    for pat in EXTRA_MD_GLOBS:
        for p in sorted(REPO_ROOT.glob(pat)):
            if p.is_file() and p.suffix == ".md" and p not in seen:
                seen.add(p); out.append(p)
    return out


def _dest_name(src: Path, taken: set[str]) -> str:
    """Output filename for *src* in the flat build/docs/ tree.

    Defensive against a future source that shares a basename with one already claimed:
    without this, two such files would silently overwrite each other in the flat output
    directory, and whichever landed last would win. The first claimant — a Doxyfile
    INPUT file, by _all_sources() ordering — keeps the plain basename; later ones get a
    path-qualified name instead of colliding."""
    if src.name not in taken:
        return src.name
    return str(src.relative_to(REPO_ROOT)).replace("/", "_")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def _fix_relative_links(text: str) -> str:
    """Rewrite markdown links that cross the source→build/ directory boundary,
    e.g.  ](docs/home_io_control.md)  becomes  ](build/docs/home_io_control.md)
    in generated build/docs/ files where Doxygen resolves against the build/ tree."""
    return text.replace(
        "](docs/home_io_control.md)",
        "](build/docs/home_io_control.md)",
    )


def main():
    DOCS_OUT_DIR.mkdir(parents=True, exist_ok=True)
    src_files = _all_sources()
    if not src_files:
        print("No Markdown files found.", file=sys.stderr)
        sys.exit(0)

    taken: set[str] = set()
    for src in src_files:
        raw = src.read_text(encoding="utf-8")
        dst_name = _dest_name(src, taken)
        taken.add(dst_name)
        dst = DOCS_OUT_DIR / dst_name

        if "```yaml" not in raw and "```yml" not in raw:
            # Files without YAML blocks are copied verbatim, but fix relative
            # links for files that cross the docs/ → build/docs/ boundary.
            if dst == DOCS_OUT_DIR / "README.md":
                raw = _fix_relative_links(raw)
            dst.write_text(raw, encoding="utf-8")
            print(f"  [copied]        {src.relative_to(REPO_ROOT)}")
            continue

        transformed = transform(raw)
        # Transformed files may also contain markdown prose links outside
        # fenced blocks; fix those post-transform.
        if dst.name == "README.md":
            transformed = _fix_relative_links(transformed)
        dst.write_text(transformed, encoding="utf-8")
        print(f"  [highlighted]   {src.relative_to(REPO_ROOT)}")

    print(f"Done. {len(src_files)} file(s) -> {DOCS_OUT_DIR}/")


if __name__ == "__main__":
    main()
