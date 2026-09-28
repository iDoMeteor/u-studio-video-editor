#!/usr/bin/env python3
"""Drags with the mouse from one point to another, slowly, over AT-SPI.

    drag.py <x0> <y0> <x1> <y1>

GTK's drag and drop on X11 (XDND) wants motion over the target and a
moment before the release; the packaging smoke's quick drag moves too fast
for it. Reuses tools/packaging-smoke/drive.py (SMOKE_DRIVE, SMOKE_OUT).
"""
import os
import sys
import time

sys.path.insert(0, os.environ["SMOKE_DRIVE"])
import drive  # noqa: E402

x0, y0, x1, y1 = map(int, sys.argv[1:5])
drive.mouse(x0, y0, "abs")
time.sleep(0.4)
drive.mouse(x0, y0, "b1p")
time.sleep(0.3)
steps = 60
for i in range(1, steps + 1):
    drive.mouse(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, "abs")
    time.sleep(0.04)
for dx in (3, -3, 2, -2, 0):  # linger over the target
    drive.mouse(x1 + dx, y1, "abs")
    time.sleep(0.15)
time.sleep(0.5)
drive.mouse(x1, y1, "b1r")
time.sleep(0.5)
drive.log(f"drag.py {x0},{y0} -> {x1},{y1}")
