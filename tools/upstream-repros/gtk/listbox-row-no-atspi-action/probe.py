"""Print the role, name and actions of the list row, the expander and the button."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from atspi_probe_common import describe, find_app, walk  # noqa: E402

app = find_app(sys.argv[1] if len(sys.argv) > 1 else "listrow")
for node in walk(app):
    if node.get_role_name() in ("list item", "button", "push button", "toggle button"):
        print(describe(node))
