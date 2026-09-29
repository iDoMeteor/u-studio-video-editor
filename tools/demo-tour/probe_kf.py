# Effects 2 part: keyframes and curve lanes, a mask, an adjustment block, a LUT. A fresh project (parts.json "script"); needs the effects
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


def seek(f):
    sl = find('Seek', roles=('slider',), timeout=3)
    if sl: sl.get_value_iface().set_current_value(float(f)); pause(0.8)
def mix_slider():
    return find('Mix', roles=('slider',), timeout=3)
def set_mix(v):
    m = mix_slider()
    if m:
        vi = m.get_value_iface(); vi.set_current_value(vi.get_minimum_value() + v * (vi.get_maximum_value() - vi.get_minimum_value())); pause(0.8)
def add_effect(query, name):
    act('effects-browser'); pause(2)
    s = find('Search effects', roles=('entry', 'text'), timeout=2)
    if s:
        click(*centre_of(s)); pause(0.3); keysym(ord('a'), mods=('ctrl',)); typestr(query); pause(2.5)
    t = tile(name)
    if t: click(*centre_of(t)); pause(2.5)
    T.log(f"add {name}: {status()!r}")
FX_Y = 762                                          # the FX lane, between the ruler and V1

# A warm .cube LUT made at runtime (17-point, lifts red, trims blue).
WORK = os.path.join(OUT, 'work'); os.makedirs(WORK, exist_ok=True)
LUT = os.path.join(WORK, 'unicorn-warm.cube')
with open(LUT, 'w') as f:
    f.write('TITLE "Unicorn warm"\nLUT_3D_SIZE 17\n')
    for b in range(17):
        for g in range(17):
            for r in range(17):
                R, G, B = r / 16, g / 16, b / 16
                f.write(f"{min(1, R * 1.12 + 0.03):.6f} {min(1, G * 1.02 + 0.01):.6f} {B * 0.85:.6f}\n")


launch(); inspector(False)
import_folder(M + '/unicorn'); pause(6); rest()
press('Add track'); pause(1.2); import_folder(M + '/zizzle'); pause(6); rest()
act('zoom-fit'); pause(0.8)
seek(15); click(60, 752 + 38); pause(0.8)          # the first zizzle clip on V2 (top row)
act('effects-browser'); pause(1.5); tab('Effects'); pause(2)
dump('kf-card')
quit_app()
