#!/usr/bin/env python3
"""Switch GPU acceleration in the running editor through AT-SPI actions only
(no pointer or keys, so it works on Wayland): Settings, the Performance page,
the GPU acceleration switch, then close Settings.
    gpu_toggle.py [open|toggle|close|all]
"""
import sys, time
import gi
gi.require_version('Atspi', '2.0')
from gi.repository import Atspi

def app():
    d = Atspi.get_desktop(0)
    for i in range(d.get_child_count()):
        a = d.get_child_at_index(i)
        if a and a.get_name() == 'u-studio-video-editor':
            return a
    return None

def walk(n, depth=0):
    if n is None or depth > 60:
        return
    yield n
    for i in range(n.get_child_count()):
        try:
            yield from walk(n.get_child_at_index(i), depth + 1)
        except Exception:
            pass

def find(pred, timeout=6):
    end = time.time() + timeout
    while time.time() < end:
        for n in walk(app()):
            try:
                if pred(n):
                    return n
            except Exception:
                pass
        time.sleep(0.3)
    return None

def do(n, what):
    ai = n.get_action_iface()
    for i in range(ai.get_n_actions()):
        if ai.get_action_name(i) in ('click', 'activate', 'press', 'toggle', 'select'):
            ai.do_action(i); print(f'{what}: {ai.get_action_name(i)}', flush=True); return True
    print(f'{what}: no action', flush=True); return False

step = sys.argv[1] if len(sys.argv) > 1 else 'all'
if step in ('open', 'all'):
    b = find(lambda n: n.get_role_name() == 'button' and n.get_name() == 'Settings')
    do(b, 'Settings') if b else print('no Settings button')
    time.sleep(2)
    p = find(lambda n: n.get_name() == 'Performance' and n.get_role_name() in ('toggle button', 'page tab', 'radio button', 'button', 'list item'))
    do(p, 'Performance') if p else print('no Performance page')
    time.sleep(2)
if step in ('toggle', 'all'):
    # The row and its switch both say "GPU acceleration"; only the switch acts.
    s = find(lambda n: n.get_role_name() == 'switch' and n.get_name().startswith('GPU acceleration')
             and n.get_action_iface() is not None and n.get_action_iface().get_n_actions() > 0)
    do(s, 'GPU switch') if s else print('no GPU switch')
    time.sleep(2.5)
if step in ('close', 'all'):
    dlg = find(lambda n: n.get_name() == 'Preferences' and n.get_role_name() in ('dialog', 'frame', 'panel', 'filler'), timeout=2)
    c = None
    if dlg:
        for n in walk(dlg):
            if n.get_role_name() == 'button' and n.get_name() == 'Close':
                c = n
    do(c, 'Close Settings') if c else print('no Settings close button')
