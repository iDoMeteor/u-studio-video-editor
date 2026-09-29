"""Drag slowly from the "Drag me" label to the content with synthetic mouse events."""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from atspi_probe_common import Atspi, find_app, walk  # noqa: E402

nodes = walk(find_app(sys.argv[1] if len(sys.argv) > 1 else "shield"))


def centre(name):
    node = [n for n in nodes if n.get_name() == name and n.get_role_name() == "label"][0]
    e = node.get_component_iface().get_extents(Atspi.CoordType.SCREEN)
    return e.x + e.width // 2, e.y + e.height // 2


def mouse(x, y, event):
    Atspi.generate_mouse_event(int(x), int(y), event)


(x0, y0) = centre("Drag me")
(x1, y1) = (x0 + 350, y0)  # a point in the content, right of the 200 px sidebar
mouse(x0, y0, "abs"); time.sleep(0.4)
mouse(x0, y0, "b1p"); time.sleep(0.3)
for i in range(1, 61):  # XDND wants motion over the target and a pause before the release
    mouse(x0 + (x1 - x0) * i / 60, y0 + (y1 - y0) * i / 60, "abs"); time.sleep(0.04)
for dx in (3, -3, 2, -2, 0):
    mouse(x1 + dx, y1, "abs"); time.sleep(0.15)
time.sleep(0.5)
mouse(x1, y1, "b1r"); time.sleep(1)
print(f"dragged {x0},{y0} -> {x1},{y1}; see program.log")
