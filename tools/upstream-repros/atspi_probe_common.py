"""Helpers shared by the AT-SPI probes: find the program, walk its tree."""
import sys
import time

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi  # noqa: E402


def find_app(name):
    for _ in range(80):
        desktop = Atspi.get_desktop(0)
        for i in range(desktop.get_child_count()):
            a = desktop.get_child_at_index(i)
            if a and a.get_name() and name in a.get_name():
                time.sleep(1)
                return a
        time.sleep(0.25)
    sys.exit(f"{name} not found on the AT-SPI bus")


def walk(node, out=None):
    out = [] if out is None else out
    out.append(node)
    for i in range(node.get_child_count()):
        c = node.get_child_at_index(i)
        if c:
            walk(c, out)
    return out


def actions(node):
    a = node.get_action_iface()
    return [a.get_action_name(i) for i in range(a.get_n_actions())] if a else []


def describe(node):
    s = node.get_state_set()
    states = [n for n in ("expandable", "expanded") if s.contains(getattr(Atspi.StateType, n.upper()))]
    return f"[{node.get_role_name()}] name={node.get_name()!r} actions={actions(node)} {' '.join(states)}".rstrip()
