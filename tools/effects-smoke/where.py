#!/usr/bin/env python3
"""Prints the centre of the first widget named <name>, in window
coordinates (the screen's, with the window at the Xvfb screen's top left).

    where.py <name> [index]

Reuses tools/packaging-smoke/drive.py (SMOKE_DRIVE, SMOKE_OUT).
"""
import os
import sys
import time

sys.path.insert(0, os.environ["SMOKE_DRIVE"])
import drive  # noqa: E402
from gi.repository import Atspi  # noqa: E402

name = sys.argv[1]
index = int(sys.argv[2]) if len(sys.argv) > 2 else 0
for _ in range(20):
    try:
        found = [n for n in drive.walk(drive.app_node()) if drive.info(n)[0] == name]
        if len(found) > index:
            e = found[index].get_extents(Atspi.CoordType.WINDOW)
            if e.width > 0 and e.height > 0:
                print(f"{e.x + e.width // 2} {e.y + e.height // 2}")
                sys.exit(0)
    except Exception:
        pass
    time.sleep(0.3)
sys.exit(1)
