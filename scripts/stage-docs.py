#!/usr/bin/env python3
r"""Markdown staging pipeline for doxygen.

The single place where committed, 100% GitHub-flavoured Markdown becomes the
doxygen-flavoured Markdown that `Doxyfile` INPUT points at (under `build/docs/`).
Nothing doxygen-only is ever committed to a source `.md` file -- the doxygen
syntax is generated here. Run by `scripts/generate-doxygen.sh` before doxygen.

Transforms, applied in this order. All are newline-neutral: the staged copy has
the same number of lines as the source, so doxygen's warning line numbers still
point at the right line of the committed file.

  1. YAML fence highlighting -- ```yaml / ```yml fences become Pygments HTML
     (doxygen has no YAML lexer). The replacement is padded with blank
     `<div class="line">` rows to keep the original block's line count.

  2. Page-label injection -- an invisible `<!-- doxygen-label: X -->` comment
     anywhere in the file appends `{#X}` to the first H1, so the page gets a
     short, stable URL (`X.html`) that does not depend on the file's path.

  3. Subpage-list generation -- Markdown bullets between
     `<!-- doxygen-subpages -->` and `<!-- /doxygen-subpages -->` become
     `\subpage <label>` lines (one per bullet), building a real nested page tree.
     GitHub renders the bullet list; doxygen gets the tree. The label of each
     child is read from that child file's own `doxygen-label` comment, so the
     list cannot drift from the labels.

  4. Link rewriting -- a small table (see `rewrite_links`):
       * `](docs/x.md#f)` (a README link across the source -> staged boundary)
         becomes `](build/docs/x.md#f)`.
       * `[text](https://<pages-site>/group__<g>.html)` -- a GitHub-friendly
         absolute link to one of this project's own API group pages -- becomes
         doxygen's own `\ref <g> "text"`. (A bare relative `x.html` link does
         NOT work: doxygen treats it as an unresolved \ref, so the round trip
         has to go back to \ref, not to a relative path.)

Transforms 2-4 act outside fenced code blocks only.

Output paths mirror the source tree with a leading `docs/` removed:
  README.md         -> build/docs/README.md
  docs/foo.md       -> build/docs/foo.md
  docs/adr/0001.md  -> build/docs/adr/0001.md
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

from md_fences import sub_outside_fences

SCRIPT_DIR = Path(__file__).parent.resolve()
REPO_ROOT = SCRIPT_DIR.parent
DOCS_OUT_DIR = REPO_ROOT / "build" / "docs"

# Source globs, relative to the repo root. Order does not matter: each file's
# output path comes from its own path, and its URL comes from its label.
SOURCE_GLOBS = [
    "README.md",
    "docs/*.md",
    "docs/adr/*.md",
]

# The published GitHub Pages site (transform 4). Keep in sync with the README
# link and scripts/check-doxygen-output-links.py.
PAGES_BASE_URL = "https://laberning.github.io/home_io_control/"

# transform 4a: README links that cross the docs/ -> build/docs/ staging boundary.
_BOUNDARY_LINK_RE = re.compile(r"\]\(docs/([^)]+\.md(?:#[^)]*)?)\)")

# transform 4b: a GitHub-friendly absolute link to one of our own API group pages,
# turned back into doxygen's \ref. group__hioc__protocol.html -> \ref hioc_protocol.
_GROUP_LINK_RE = re.compile(
    r"\[([^\]]+)\]\(" + re.escape(PAGES_BASE_URL) + r"group__([a-z0-9_]+)\.html\)"
)

_LABEL_RE = re.compile(r"<!--\s*doxygen-label:\s*([A-Za-z0-9_]+)\s*-->")
_SUBPAGES_BLOCK_RE = re.compile(
    r"<!--\s*doxygen-subpages\s*-->\n(.*?)\n<!--\s*/doxygen-subpages\s*-->",
    re.DOTALL,
)
_BULLET_RE = re.compile(r"^\s*[-*]\s+\[[^\]]*\]\(([^)#]+\.md)(?:#[^)]*)?\)\s*$")
_H1_RE = re.compile(r"^#\s+\S")


# ---------------------------------------------------------------------------
# Transform 1: YAML fence highlighting (unchanged behaviour)
# ---------------------------------------------------------------------------

# Matches fenced yaml blocks; the closing fence re-uses group(1)'s backtick count.
_FENCE_RE = re.compile(r"^(```+)\s*(yaml|yml)\s*\n(.+?)\n\1", re.DOTALL | re.MULTILINE)


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


def _replace_block(m: re.Match) -> str:
    """Replace one matched yaml fence with HTML that occupies the same number
    of file lines as the original fence block, preserving doxygen line refs."""
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

    # File-lines occupied by the original match: count \n in the match text + 1
    # (a 0-\n match spans 1 line).
    orig_file_lines = m.group(0).count("\n") + 1
    n_html_lines = html.count("\n") + 1

    pad = max(0, orig_file_lines - n_html_lines)
    if pad == 0:
        return html

    # Pad with invisible <div class="line"> rows so the output keeps the original
    # block's height in the file (doxygen-awesome renders each as one line).
    padding = "\n".join(['<div class="line"><span></span></div>'] * pad)
    return f"{html}\n{padding}"


def highlight_yaml(content: str) -> str:
    return _FENCE_RE.sub(_replace_block, content)


# ---------------------------------------------------------------------------
# Transform 2: page-label injection
# ---------------------------------------------------------------------------

def _label_for(text: str, src: Path) -> str | None:
    """The `doxygen-label` value declared in *text*, or None. Fails loudly on
    a second, conflicting declaration."""
    labels = _LABEL_RE.findall(text)
    if not labels:
        return None
    if len(set(labels)) > 1:
        sys.exit(f"stage-docs: {_rel(src)} declares conflicting doxygen-labels: {sorted(set(labels))}")
    return labels[0]


def inject_label(content: str, src: Path) -> str:
    label = _label_for(content, src)
    if label is None:
        return content
    lines = content.split("\n")
    in_fence = False
    for i, line in enumerate(lines):
        if line.lstrip().startswith(("```", "~~~")):
            in_fence = not in_fence
            continue
        if not in_fence and _H1_RE.match(line):
            lines[i] = f"{line.rstrip()} {{#{label}}}"
            return "\n".join(lines)
    sys.exit(f"stage-docs: {_rel(src)} has a doxygen-label but no H1 to attach {{#{label}}} to")


# ---------------------------------------------------------------------------
# Transform 3: subpage-list generation
# ---------------------------------------------------------------------------

def generate_subpages(content: str, src: Path) -> str:
    seen: dict[str, str] = {}  # label -> bullet text, for duplicate detection

    def _one_block(block_match: re.Match) -> str:
        inner = block_match.group(1)
        out_lines: list[str] = []
        for line in inner.split("\n"):
            bullet = _BULLET_RE.match(line)
            if bullet is None:
                out_lines.append(line)  # blank lines / stray prose pass through
                continue
            target = (src.parent / bullet.group(1)).resolve()
            if not target.is_file():
                sys.exit(f"stage-docs: {_rel(src)} subpage bullet points at missing file: {bullet.group(1)}")
            child_label = _label_for(target.read_text(encoding="utf-8"), target)
            if child_label is None:
                sys.exit(f"stage-docs: {_rel(src)} subpages {bullet.group(1)}, which has no doxygen-label")
            if child_label in seen:
                sys.exit(f"stage-docs: doxygen-label '{child_label}' claimed twice in {_rel(src)}'s subpage list")
            seen[child_label] = line
            out_lines.append(re.sub(r"\S.*", rf"\\subpage {child_label}", line, count=1))
        return "<!-- doxygen-subpages -->\n" + "\n".join(out_lines) + "\n<!-- /doxygen-subpages -->"

    return _SUBPAGES_BLOCK_RE.sub(_one_block, content)


# ---------------------------------------------------------------------------
# Transform 4: link rewriting
# ---------------------------------------------------------------------------

def _group_link_to_ref(m: re.Match) -> str:
    text, mangled = m.group(1), m.group(2)
    # group__hioc__protocol -> hioc_protocol  (doxygen doubles every '_' when it
    # mangles the @defgroup name into a filename).
    group = mangled.replace("__", "_")
    return rf'\ref {group} "{text}"'


def rewrite_links(content: str) -> str:
    content = sub_outside_fences(_BOUNDARY_LINK_RE, r"](build/docs/\1)", content)
    content = sub_outside_fences(_GROUP_LINK_RE, _group_link_to_ref, content)
    return content


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def _rel(p: Path) -> str:
    try:
        return str(p.relative_to(REPO_ROOT))
    except ValueError:
        return str(p)


def _dest_path(src: Path) -> Path:
    rel = src.relative_to(REPO_ROOT)
    if rel.parts and rel.parts[0] == "docs":
        rel = Path(*rel.parts[1:])
    return DOCS_OUT_DIR / rel


def _sources() -> list[Path]:
    seen: set[Path] = set()
    out: list[Path] = []
    for pattern in SOURCE_GLOBS:
        for p in sorted(REPO_ROOT.glob(pattern)):
            if p.is_file() and p.suffix == ".md" and p not in seen:
                seen.add(p)
                out.append(p)
    return out


def stage(src: Path) -> str:
    raw = src.read_text(encoding="utf-8")
    staged = raw
    staged = highlight_yaml(staged)
    staged = inject_label(staged, src)
    staged = generate_subpages(staged, src)
    staged = rewrite_links(staged)

    if staged.count("\n") != raw.count("\n"):
        sys.exit(
            f"stage-docs: {_rel(src)} line count changed in staging "
            f"({raw.count(chr(10))} -> {staged.count(chr(10))}); a transform is not newline-neutral"
        )
    return staged


def main() -> None:
    sources = _sources()
    if not sources:
        sys.exit("stage-docs: no source Markdown found")

    # build/docs/ holds only staged output -- wipe it so a renamed or deleted
    # source cannot leave a stale copy behind for doxygen to pick up.
    shutil.rmtree(DOCS_OUT_DIR, ignore_errors=True)

    for src in sources:
        dst = _dest_path(src)
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(stage(src), encoding="utf-8")
        print(f"  [staged] {_rel(src)} -> {_rel(dst)}")

    print(f"Done. {len(sources)} file(s) -> {_rel(DOCS_OUT_DIR)}/")


if __name__ == "__main__":
    main()
