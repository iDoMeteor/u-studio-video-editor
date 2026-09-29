"""Open the menu through the menu button's action, then print each menu item's
name, its labelled-by targets and its children; activate the first item."""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from atspi_probe_common import describe, find_app, walk  # noqa: E402

name = sys.argv[1] if len(sys.argv) > 1 else "menuitem"
app = find_app(name)
toggle = [n for n in walk(app) if n.get_role_name() == "toggle button" and n.get_name() == "Menu"][0]
toggle.get_action_iface().do_action(0)
time.sleep(1.5)
items = [n for n in walk(find_app(name)) if n.get_role_name() == "menu item"]
for item in items:
    labelled_by = [r.get_target(i).get_name() for r in item.get_relation_set()
                   if r.get_relation_type().value_nick == "labelled-by" for i in range(r.get_n_targets())]
    print(describe(item), f"labelled-by={labelled_by}")
    for child in walk(item)[1:]:
        print("    child", describe(child))
items[0].get_action_iface().do_action(0)
time.sleep(1)
print("activated the first item; see program.log")
