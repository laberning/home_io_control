#!/usr/bin/env python3
"""Detect (and optionally clean) stale ESPHome test build caches.

`config/tests/.esphome/build/<env>/` directories can survive across sessions. Regenerating
the ESPHome C++ source tree copies a newly added `components/home_io_control/*.cpp` file
in, but the underlying object/dependency cache doesn't always notice — so the new file's
object is silently left out of the compile+link set, producing a confusing
"undefined reference to vtable" linker error from `make clang-tidy` / `make firmware-test`
even though `make unit-test` (the host stub build) is unaffected. A genuinely fresh env
(never built) never hits this; it only bites pre-existing build directories.

This script compares the current `components/home_io_control/*.cpp` list against the
compiled objects each env actually has (matching both `*.cpp.o` and `*.cpp.obj` -- the
on-disk suffix has already drifted once, from SCons to CMake, so don't assume either).
Any current source with no matching object -- or an env whose build tree exists but has
zero home_io_control objects at all (a half-regenerated tree) -- is reported stale.

Run via `make check-build-cache`; wired automatically (with --clean) into the first line
of `make firmware-test` and near the top of `scripts/run-clang-tidy.sh`, so this gotcha is
now caught and fixed before it costs a debugging session.
"""

import argparse
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Set

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BUILD_ROOT = REPO_ROOT / "config" / "tests" / ".esphome" / "build"
DEFAULT_COMPONENTS_DIR = REPO_ROOT / "components" / "home_io_control"


def current_source_stems(components_dir: Path) -> Set[str]:
    """Every `components/home_io_control/*.cpp` file, by stem (no extension)."""
    return {p.stem for p in components_dir.glob("*.cpp")}


def compiled_stems(env_dir: Path) -> Optional[Set[str]]:
    """Home_io_control source stems with a compiled object under env_dir.

    Returns None if env_dir has no `build/` subdirectory yet -- a fresh compile builds
    everything, so that's not stale, just not started.
    """
    build_dir = env_dir / "build"
    if not build_dir.is_dir():
        return None
    stems = set()
    for pattern in ("*.cpp.o", "*.cpp.obj"):
        for obj in build_dir.rglob(f"esphome/components/home_io_control/{pattern}"):
            stems.add(obj.name.split(".cpp.")[0])
    return stems


def stale_sources(env_dir: Path, current: Set[str]) -> Optional[List[str]]:
    """Missing source stems for env_dir, or None if env_dir isn't stale.

    An env with a build/ dir but zero home_io_control objects at all is reported as fully
    stale (same conservative call the Makefile's clean-test-cache comment describes for a
    half-regenerated tree).
    """
    compiled = compiled_stems(env_dir)
    if compiled is None:
        return None
    if not compiled:
        # Build tree exists but never produced a single home_io_control object -- a
        # half-regenerated tree. Not stale only if there's nothing it should have compiled.
        return sorted(current) if current else None
    missing = sorted(current - compiled)
    return missing if missing else None


def clean_env(env_name: str) -> bool:
    cmd = [
        "docker", "compose", "run", "--rm", "--entrypoint", "sh", "esphome",
        "-c", f"rm -rf /config/tests/.esphome/build/{env_name}",
    ]
    result = subprocess.run(cmd, cwd=REPO_ROOT)
    return result.returncode == 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-root", type=Path, default=DEFAULT_BUILD_ROOT,
                        help="Directory containing per-env build trees (default: %(default)s)")
    parser.add_argument("--components-dir", type=Path, default=DEFAULT_COMPONENTS_DIR,
                        help="Directory of current component sources (default: %(default)s)")
    parser.add_argument("--clean", action="store_true",
                        help="Delete stale env build dirs via Docker instead of just reporting them")
    args = parser.parse_args()

    if not args.build_root.is_dir():
        print(f"check-build-cache: {args.build_root} does not exist yet -- nothing to check")
        return 0

    current = current_source_stems(args.components_dir)
    env_dirs = sorted(p for p in args.build_root.iterdir() if p.is_dir())

    stale: Dict[str, List[str]] = {}
    for env_dir in env_dirs:
        missing = stale_sources(env_dir, current)
        if missing is not None:
            stale[env_dir.name] = missing

    for env_dir in env_dirs:
        if env_dir.name in stale:
            missing = stale[env_dir.name]
            detail = ", ".join(missing) if missing else "no home_io_control objects at all"
            print(f"check-build-cache: STALE {env_dir.name} (missing: {detail})")
        else:
            print(f"check-build-cache: clean {env_dir.name}")

    if not stale:
        print("check-build-cache: OK (all envs clean)")
        return 0

    if args.clean:
        for name in stale:
            print(f"check-build-cache: cleaning {name}...")
            if not clean_env(name):
                print(f"check-build-cache: failed to clean {name}", file=sys.stderr)
                return 1
        print("check-build-cache: cleaned all stale envs")
        return 0

    print("check-build-cache: stale envs detected -- fix with:", file=sys.stderr)
    print("  python3 scripts/check-build-cache.py --clean", file=sys.stderr)
    print("  (or make clean-test-cache to wipe everything)", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
