# Effects 1 part: the Add page, trying effects on the picture, the Effects page,
# Looks, and Compare. A fresh project (parts.json "script"); needs the effects
# drop-in. Runs as TOUR_SCRIPT under run_tour.sh.
import os, sys, time
sys.path.insert(0, os.environ['TOUR_DEMO'])
import tourlib as T
from tourlib import *
M = os.environ['TOUR_MEDIA']
OUT = os.environ['TOUR_OUT']
WORK = os.path.join(OUT, 'work')
SHOTS = os.environ.get('TOUR_SHOTS', '1') == '1'
STOP_AFTER = os.environ.get('TOUR_STOP_AFTER', '')
def snap(n):
    if SHOTS: shot(n)
def dump(name):
    if SHOTS: dump_tree(os.path.join(OUT, f'tree-{name}.txt'))
def is_on(node):
    st = node.get_state_set()
    return st.contains(Atspi.StateType.PRESSED) or st.contains(Atspi.StateType.CHECKED)
def inspector(open_=False):
    n = find('Inspector', roles=('toggle button',), timeout=4)
    if n and is_on(n) != open_:
        click(*centre_of(n)); pause(0.8)
def import_folder(folder):
    act('import'); center_dialogs(1.0)
    set_location(folder.rstrip('/') + '/'); keysym(K_RETURN); pause(1.5)
    (dx, dy), dw, dh = dialog_origin()
    click(dx + 300, dy + 93); keysym(ord('a'), mods=('ctrl',)); pause(0.8)
    press('Open', exact=True)
def import_file(path):
    act('import'); center_dialogs(1.0); set_location(path); keysym(K_RETURN); pause(1.2)
    if find('Open', exact=True, timeout=1):
        press('Open', exact=True)
def wait_status(prefixes, timeout):
    end = time.time() + timeout
    while time.time() < end:
        st = status()
        if st.startswith(prefixes):
            return st
        time.sleep(0.5)
    return status()

last = None
def step(title, sub=''):
    global last
    if STOP_AFTER and last == STOP_AFTER:
        narration_wait(); pause(1.0); chapter('(end)'); quit_app(); sys.exit(0)
    last = title
    chapter(title, sub)


def tile(name):
    return find(name, roles=('table cell',), exact=True, timeout=4)
def tab(name):
    t = find(name, roles=('page tab',), exact=True, timeout=3)
    if t: click(*centre_of(t)); pause(1.2)


launch(); inspector(False)
import_folder(M + '/unicorn'); pause(6); rest()
seek = lambda f: find('Seek', roles=('slider',), timeout=3).get_value_iface().set_current_value(float(f))
seek(60); pause(1)
click(120, 752 + 38); pause(0.8)
act('effects-browser'); pause(3)
s = find('Search effects', roles=('entry', 'text'), timeout=2)
click(*centre_of(s)); pause(0.3); typestr('glow'); pause(2.5)
click(*centre_of(tile('Glow'))); pause(2.5)
tab('Effects'); pause(2); dump('e2-rack')
act('effects-curve-lanes'); pause(2); dump('e2-lanes')
act('effects-curve-lanes'); pause(1)
dump('e2-timeline')
quit_app()
