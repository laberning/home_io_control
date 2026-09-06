#!/usr/bin/env python3
"""Fenced-code-block awareness for the Markdown tooling.

`scripts/check-docs-links.py` (link/anchor checker) and `scripts/stage-docs.py`
(doxygen staging pipeline) both need to treat ``` / ~~~ fenced blocks as opaque
-- a `#` comment or a `[text](link)` inside an example is not a heading or a
link. This is the one shared definition of "what a fence is" so the two scripts
cannot drift.

Not a full CommonMark parser: it assumes the closing fence repeats the opening
marker run, which holds for this repo's docs.
"""

from __future__ import annotations

import re

# Opening fence (>=3 of ` or ~) on its own line, through the matching closing run.
FENCE_RE = re.compile(r"^(```+|~~~+)[^\n]*\n.*?^\1[^\n]*$", re.DOTALL | re.MULTILINE)


def strip_fenced_blocks(text: str) -> str:
    """Return *text* with every fenced block replaced by an empty string.

    For scanners that only look for headings/links and never need the code.
    """
    return FENCE_RE.sub("", text)


def sub_outside_fences(pattern: "re.Pattern[str] | str", repl, text: str) -> str:
    """`re.sub(pattern, repl, ...)` applied only to the parts of *text* that are
    outside fenced code blocks; fenced blocks are copied through untouched.

    Callers that rely on line numbers staying stable (the staging pipeline) must
    still ensure *repl* itself is newline-neutral -- this helper preserves the
    fence segments verbatim but does not police the replacement.
    """
    if isinstance(pattern, str):
        pattern = re.compile(pattern)
    out: list[str] = []
    pos = 0
    for m in FENCE_RE.finditer(text):
        out.append(pattern.sub(repl, text[pos : m.start()]))
        out.append(m.group(0))
        pos = m.end()
    out.append(pattern.sub(repl, text[pos:]))
    return "".join(out)
