#!/usr/bin/env python3
"""Fail unless the metainfo has release notes for meson.build's version.

    tools/check_release_notes.py

A packaged build is a releasable build (CLAUDE.md, "Git conventions"): its
version needs a <release> entry in data/com.ustudio.VideoEditor.metainfo.xml,
which Help > Release notes and software centres show. Without one, Flatpak
reports the newest entry's version instead (0.65.0 and 0.67.1 shipped that
way). `just flatpak`, `just flatpak-titles` and tools/flathub_prep.py run
this first.
"""
import pathlib
import re
import sys
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parent.parent
METAINFO = ROOT / "data" / "com.ustudio.VideoEditor.metainfo.xml"


def project_version() -> str:
    match = re.search(r"^  version: '([^']+)',$", (ROOT / "meson.build").read_text(), re.M)
    if not match:
        raise SystemExit("meson.build: no `version:` line found")
    return match.group(1)


def check() -> str | None:
    """None when the notes are there, else what to do about it."""
    version = project_version()
    releases = [r.get("version") for r in ET.parse(METAINFO).getroot().iter("release")]
    if version in releases:
        return None
    return (f"{METAINFO.relative_to(ROOT)} has no <release version=\"{version}\">: add a <releases> entry "
            f"for {version}, written for testers, before packaging (CLAUDE.md, \"Git conventions\"). "
            f"Newest entry: {releases[0] if releases else 'none'}.")


def main() -> int:
    problem = check()
    if problem:
        print(problem, file=sys.stderr)
        return 1
    print(f"release notes: {project_version()} is in the metainfo")
    return 0


if __name__ == "__main__":
    sys.exit(main())
