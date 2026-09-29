#!/usr/bin/env python3
"""Chooses an option of a drop-down (GtkDropDown) over AT-SPI: opens it,
moves `steps` items down from the current one with the arrow key, and
presses Enter (the list's rows take no AT-SPI action). A drop-down's
accessible name is its selected item's ("None"), so `index` picks among
those (-1: the last).

    choose.py <drop-down name> <steps> [index]

Reuses tools/packaging-smoke/drive.py (SMOKE_DRIVE, SMOKE_OUT).
"""
import os
import sys
import time

sys.path.insert(0, os.environ["SMOKE_DRIVE"])
import drive  # noqa: E402

name, steps = sys.argv[1], int(sys.argv[2])
index = int(sys.argv[3]) if len(sys.argv) > 3 else 0


def act(node):
    for i, action in enumerate(drive.action_names(node)):
        if action in ("click", "activate", "press", "toggle"):
            node.get_action_iface().do_action(i)
            return True
    return False


for _ in range(10):
    try:
        boxes = [n for n in drive.walk(drive.app_node()) if drive.info(n) == (name, "combo box")]
        # The drop-down's own node is labelled; the button that opens it is
        # inside it (walk() yields the node itself first).
        if boxes and any(act(inner) for inner in drive.walk(boxes[index])):
            break
    except Exception:
        pass
    time.sleep(0.3)
else:
    drive.log(f"choose '{name}': NOT FOUND")
    sys.exit(1)
time.sleep(0.8)
for _ in range(steps):
    drive.keysym(0xFF54)  # Down
    time.sleep(0.2)
drive.keysym(0xFF0D)  # Enter
drive.log(f"choose '{name}': {steps} down, Enter")
