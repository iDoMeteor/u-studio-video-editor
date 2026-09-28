#!/usr/bin/env python3
"""Selects the n-th tile of the effects Browser's grid, over AT-SPI.

    tile.py <index>

Selecting a tile is what the arrow keys do: the Browser auditions that
effect on the preview. Reuses tools/packaging-smoke/drive.py (SMOKE_DRIVE,
SMOKE_OUT).
"""
import os
import sys
import time

sys.path.insert(0, os.environ["SMOKE_DRIVE"])
import drive  # noqa: E402

index = int(sys.argv[1])
error = "NOT FOUND"
for _ in range(20):
    try:
        for node in drive.walk(drive.app_node()):
            sel = node.get_selection_iface()
            if not sel or node.get_child_count() <= index:
                continue
            names = [drive.info(node.get_child_at_index(i))[0] for i in range(node.get_child_count())]
            if "Glow" in names and sel.select_child(index):
                drive.log(f"select tile {index}: {names[index]}")
                sys.exit(0)
    except Exception as e:
        error = str(e)
    time.sleep(0.3)
drive.log(f"select tile {index}: {error}")
sys.exit(1)
