#!/usr/bin/env python3
"""Fail if a built package tree carries test or build-machine artefacts.

    tools/check_bundle_clean.py <tree> [--home <dir>]

<tree> is what ships: a Flatpak build's files/ directory, or a Snap's prime/.
A distro package must not contain test projects, media, logs, autosaves,
user settings, or paths from the machine it was built on. Everything found
is listed, then the exit status is 1. `just flatpak` runs this before it
bundles.
"""
import argparse
import os
import pathlib
import sys

# Project files, media and raw dumps: the app installs none of these.
FORBIDDEN_SUFFIXES = {
    ".ustudio", ".mlt", ".kdenlive", ".mp4", ".mov", ".mkv", ".webm", ".avi", ".m4v", ".mts",
    ".wav", ".flac", ".mp3", ".ogg", ".opus", ".aac", ".m4a", ".raw", ".log",
}
# Images are fine as icons and store metadata; anywhere else they're a leftover.
IMAGE_SUFFIXES = {".png", ".jpg", ".jpeg", ".webp", ".gif", ".bmp", ".tif", ".tiff"}
IMAGE_DIRS = ("share/icons/", "share/app-info/", "share/metainfo/", "share/pixmaps/", "meta/gui/", "snap/gui/")
# Per-user state the app writes at run time (XDG state/config/cache).
FORBIDDEN_DIR_NAMES = {"autosave", ".ustudio-backups", ".var", "dconf"}
# Strings that only a test run or this build machine would leave behind.
MARKERS = [b"ustudio-flatpak-smoke", b"ustudio-smoke-home", b"ustudio-smoke-media", b"/tmp/claude-", b".var/app/"]
# flatpak-builder moves debug info (and the sources it points at) here; the
# export splits it into the separate .Debug extension, so it never ships in
# the app or the bundle.
SKIP_PREFIXES = ("lib/debug/",)
MAX_SCAN_BYTES = 256 * 1024 * 1024


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("tree", type=pathlib.Path)
    parser.add_argument("--home", default=os.path.expanduser("~"),
                        help="build machine home directory to look for (default: $HOME)")
    args = parser.parse_args()
    if not args.tree.is_dir():
        print(f"{args.tree}: not a directory", file=sys.stderr)
        return 2
    markers = MARKERS + [args.home.rstrip("/").encode() + b"/"]

    problems = []
    files = 0
    for path in sorted(args.tree.rglob("*")):
        rel = path.relative_to(args.tree).as_posix()
        if rel.startswith(SKIP_PREFIXES) or rel + "/" in SKIP_PREFIXES:
            continue
        if path.is_dir():
            if path.name in FORBIDDEN_DIR_NAMES:
                problems.append(f"{rel}/: per-user state directory")
            continue
        if path.is_symlink() or not path.is_file():
            continue
        files += 1
        suffix = path.suffix.lower()
        if suffix in FORBIDDEN_SUFFIXES:
            problems.append(f"{rel}: {suffix} file (project, media, raw or log)")
        elif suffix in IMAGE_SUFFIXES and not any(d in "/" + rel for d in IMAGE_DIRS):
            problems.append(f"{rel}: image outside the icon/metadata directories")
        if path.name == "user" and path.parent.name == "dconf":
            problems.append(f"{rel}: a user's dconf settings database")
        if path.stat().st_size > MAX_SCAN_BYTES:
            continue
        data = path.read_bytes()
        for marker in markers:
            if marker in data:
                problems.append(f"{rel}: contains {marker.decode(errors='replace')!r}")

    for problem in problems:
        print(f"UNCLEAN {problem}")
    print(f"check_bundle_clean: {files} files in {args.tree}, {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
