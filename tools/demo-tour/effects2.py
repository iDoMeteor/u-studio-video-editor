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
act('zoom-fit'); pause(0.8)
seek(30); click(120, 752 + 38); pause(0.8)

step('Keyframes', 'set a value, press the star (P) to pin it; move the playhead and change it: the effect now changes over time')
add_effect('glow', 'Glow')
tab('Effects'); pause(1.5)
seek(30); set_mix(0.0)
pin = find('Pin (P)', roles=('button',), timeout=3)
if pin: click(*centre_of(pin)); pause(1.2)
seek(150); set_mix(1.0); pause(1)
T.log(f"keyframe status {status()!r}"); snap('fx2-keyframes')
seek(20); act('play-pause'); pause(5); act('play-pause'); pause(0.8)

step('Curve lanes', 'press C: a lane under the clip shows each value’s curve; drag the dots to reshape it')
act('effects-curve-lanes'); pause(2.5); snap('fx2-lanes'); pause(2)
act('effects-curve-lanes'); pause(1)

step('Masks', 'limit an effect to a rectangle or an ellipse; soften its edge, or invert it')
add_effect('kaleido', 'Kaleidoscope')           # a strong effect, so the mask's shape shows
tab('Effects'); pause(1.5)
masks = [n for n in walk(app_node()) if info(n)[2] == 'combo box' and info(n)[0] == 'None' and 'Mask' in info(n)[1]]
T.log(f"mask combos {len(masks)}")
if masks:
    click(*centre_of(masks[-1])); pause(1.2)        # the newest effect's mask (the list is top to bottom)
    e = find_any('Ellipse', exact=True, timeout=2)
    if e: click(*centre_of(e)); pause(2)
    else: keysym(K_ESC)
seek(150); pause(1.5); snap('fx2-mask'); dump('fx2-mask')

step('Adjustment blocks', 'draw a block in the FX lane: its effects change everything beneath it for that stretch')
act('zoom-fit'); pause(0.8)
drag(520, FX_Y, 820, FX_Y, dur=1.4); pause(1.5); T.log(f"block status {status()!r}"); snap('fx2-block-drawn')
add_effect('neon', 'Neon Night')
seek(300); pause(1.5); snap('fx2-block'); dump('fx2-block')

step('LUTs', 'bring in .cube colour LUTs: each gets a tile, and pointing at it shows the grade on your picture')
click(120, 752 + 38); pause(0.8)
tab('Add')
b = find('Import LUTs', roles=('button',), timeout=3)
if b:
    click(*centre_of(b)); center_dialogs(1.2); set_location(LUT); keysym(K_RETURN); pause(1.2)
    if find('Open', exact=True, timeout=1): press('Open', exact=True)
    pause(2.5)
T.log(f"lut status {status()!r}")
s = find('Search effects', roles=('entry', 'text'), timeout=2)
if s:
    click(*centre_of(s)); pause(0.3); keysym(ord('a'), mods=('ctrl',)); keysym(K_DEL); pause(1)
sec = find('Featured', roles=('combo box',), exact=True, timeout=3)
if sec:
    click(*centre_of(sec)); pause(1.2)
    l = find_any('LUTs', exact=True, timeout=2)
    if l: click(*centre_of(l)); pause(2.5)
    else: keysym(K_ESC)
dump('fx2-lut-section')
t = find('unicorn', roles=('table cell',), timeout=3) or find('Unicorn', roles=('table cell',), timeout=1)
T.log(f"lut tile {info(t) if t else None}")
if t:
    move(*centre_of(t), dur=0.8); pause(3); snap('fx2-lut-try'); click(*centre_of(t)); pause(2)
T.log(f"lut add status {status()!r}"); snap('fx2-lut')

step('(done)', '')
