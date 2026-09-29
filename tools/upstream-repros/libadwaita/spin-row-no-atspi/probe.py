"""Print the rows the preferences group's list exposes over AT-SPI."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from atspi_probe_common import find_app, walk  # noqa: E402

for node in walk(find_app(sys.argv[1] if len(sys.argv) > 1 else "spin")):
    if node.get_role_name() == "list" and node.get_child_count() >= 2:
        print(f"list with {node.get_child_count()} children:")
        for i in range(node.get_child_count()):
            child = node.get_child_at_index(i)
            print(f"  [{child.get_role_name()}] {child.get_name()!r}")
        break
