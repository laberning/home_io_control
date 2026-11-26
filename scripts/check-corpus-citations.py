#!/usr/bin/env python3
"""Every corpus capture id cited in a tracked file must name a capture that exists.

Code comments, tests, docs, decision records and capture descriptions cite captures by id
(`somfy_awning_exchange_force_open_sx1276`). A renamed or mistyped id still reads fine and points
nowhere, so this check reads every tracked text file and reports each cited id that has no file in
tests/corpus/captures/<phase>/<id>.yaml.

A cited id is recognised by its shape, `<subject>_<phase>[_<token>...]`, with the subjects and
phases of scripts/corpus/naming.py, the module that names captures. Two forms are not citations:

- generated per-frame names (`<id>_frame7`, `<id>_frame7_bytes`) cite the id before the suffix;
- an id that is a prefix of existing ids names that family of captures
  (`somfy_izymo_dimmer_pairing_full` for the four full pairings). The stem of a glob or brace
  pattern (`somfy_smoove_oneway_{open,close}_sx1276`, `atlantic_adelis_probe_*`) is read the same
  way: it must name at least one capture.

Example ids in the corpus tools' own help and self-tests name no capture and are listed in
EXAMPLE_IDS with the file they appear in.

Run via `make corpus-citation-check` (part of `make lint`); `scripts/check_corpus_citations_test.py`
tests it. Tracked files only (`git ls-files`).
"""

from __future__ import annotations

import re
import subprocess
import sys
from collections.abc import Iterable, Iterator
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "scripts" / "corpus"))
import naming  # noqa: E402  (the corpus naming rules live next to the corpus tools)

CAPTURES_DIR = Path("tests/corpus/captures")

# Ids that name no capture on purpose: examples in a tool's usage text or a self-test's scratch capture.
# Each is allowed only in the files listed for it.
EXAMPLE_IDS: dict[str, frozenset[str]] = {
    # The schema example in the corpus README.
    "somfy_awning_exchange_position_50": frozenset({"tests/corpus/README.md"}),
    # The scratch capture the ingest self-test writes into a temporary directory.
    "synthetic_probe_scaffold_smoke": frozenset({"scripts/corpus/tests/run_tests.py"}),
    # The example in the 2.4 GHz ingest tool's usage text.
    "velux_klr200_statuspoll_example": frozenset({"scripts/corpus/ingest_2g4.py"}),
    # The scratch captures the 2.4 GHz ingest self-tests write.
    "velux_klr200_statuspoll_self_test": frozenset({"scripts/corpus/tests/run_tests.py"}),
    "velux_klr200_exchange_self_test": frozenset({"scripts/corpus/tests/run_tests.py"}),
}

# This check's own files: the script lists the example ids above, and its self-test feeds it made-up ids on
# purpose. Neither cites a capture.
OWN_FILES = frozenset({"scripts/check-corpus-citations.py", "scripts/check_corpus_citations_test.py"})

# Suffixes of generated per-frame names (corpus_generated.h): `<id>_frame7`, `<id>_frame7_bytes`.
_FRAME_SUFFIX = re.compile(r"_frame\d+(?:_bytes)?$")
# Files that are not text a person writes ids into.
_BINARY_SUFFIXES = frozenset({".png", ".jpg", ".svg", ".gz", ".whl", ".bin", ".sal", ".ico", ".pdf"})


def citation_pattern(subjects: Iterable[str], phases: Iterable[str]) -> re.Pattern[str]:
    """The regular expression that finds a cited capture id: a subject, a phase, then more tokens.

    An id may end in `_` before a glob or brace (`<stem>_*`, `<stem>_{a,b}`); the match is then the
    stem, which is_known() reads as a family.
    """
    subject_alt = "|".join(sorted((re.escape(s) for s in subjects), key=len, reverse=True))
    phase_alt = "|".join(re.escape(p) for p in phases)
    return re.compile(rf"(?<![a-z0-9_])((?:{subject_alt})_(?:{phase_alt})(?:_[a-z0-9]+)*)(?![a-z0-9])")


def cited_ids(text: str, pattern: re.Pattern[str]) -> Iterator[str]:
    """Each capture id the text cites, with a generated per-frame suffix removed."""
    for match in pattern.finditer(text):
        yield _FRAME_SUFFIX.sub("", match.group(1))


def is_known(cited: str, captures: frozenset[str]) -> bool:
    """True when the id names a capture, or is the prefix of a family of captures (`<id>_...`)."""
    if cited in captures:
        return True
    family = cited + "_"
    return any(c.startswith(family) for c in captures)


def broken_citations(files: Iterable[tuple[str, str]], captures: frozenset[str], pattern: re.Pattern[str],
                     examples: dict[str, frozenset[str]]) -> list[tuple[str, int, str]]:
    """Every (file, line, id) whose cited id names no capture.

    @param files (path, text) of each file to read.
    @param captures The ids that exist.
    @param examples Example ids and the files each may appear in.
    """
    broken = []
    for path, text in files:
        for line_no, line in enumerate(text.splitlines(), start=1):
            for cited in cited_ids(line, pattern):
                if is_known(cited, captures) or path in examples.get(cited, frozenset()):
                    continue
                broken.append((path, line_no, cited))
    return broken


def tracked_text_files(root: Path) -> Iterator[tuple[str, str]]:
    """(path, text) of every tracked file that reads as UTF-8 text, this check's own files (OWN_FILES) left out."""
    listing = subprocess.run(["git", "ls-files", "-z"], cwd=root, capture_output=True, check=True).stdout
    for name in listing.decode().split("\0"):
        path = root / name
        if not name or name in OWN_FILES or Path(name).suffix.lower() in _BINARY_SUFFIXES or not path.is_file():
            continue
        try:
            yield name, path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue


def existing_captures(root: Path) -> frozenset[str]:
    """The id of every tracked capture: its file name without `.yaml`."""
    listing = subprocess.run(["git", "ls-files", "-z", str(CAPTURES_DIR)], cwd=root, capture_output=True,
                             check=True).stdout
    return frozenset(Path(n).stem for n in listing.decode().split("\0") if n.endswith(".yaml"))


def main() -> int:
    captures = existing_captures(REPO_ROOT)
    pattern = citation_pattern(naming.SUBJECTS, naming.PHASES)
    broken = broken_citations(tracked_text_files(REPO_ROOT), captures, pattern, EXAMPLE_IDS)
    for path, line_no, cited in broken:
        print(f"{path}:{line_no}: cites capture '{cited}', which does not exist in {CAPTURES_DIR}/")
    if broken:
        print(f"{len(broken)} broken capture citation(s).")
        return 1
    print(f"Corpus capture citations OK ({len(captures)} captures).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
