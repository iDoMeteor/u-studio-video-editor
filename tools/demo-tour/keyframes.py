# Keyframed transforms part: the Transform card's pins and Feel, key-aware
# handles on the preview, curve lanes, touch-record. A fresh project (parts.json "script"); needs the effects
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


def spin(name):
    return find(name, roles=('spin button',), exact=True, timeout=3)
def set_spin(name, v):
    n = spin(name)
    if n: n.get_value_iface().set_current_value(float(v)); pause(0.5)
    return bool(n)
def row_button(name, which):
    """The Pin / Touch-record button on the same row as spin button `name`."""
    n = spin(name)
    if not n: return None
    y = extents(n)[1]
    for b in walk(app_node()):
        nm, ds, rl = info(b)
        if rl in ('button', 'toggle button') and nm.startswith(which) and abs(extents(b)[1] - y) < 4:
            return b
    return None
A, B = 15, 105                                        # key frames on the clip: 0:00.5 and 0:03.5
def sel_v2(frame):
    """Select the PiP clip (V2, top row), then seek: the click moves the playhead."""
    click(60, 752 + 38); pause(0.8); seek(frame); pause(0.8)

launch(); inspector(False)
import_folder(M + '/unicorn'); pause(6); rest()
press('Add track'); pause(1.2); import_folder(M + '/zizzle'); pause(6); rest()
act('zoom-fit'); pause(0.8)
click(60, 752 + 38); pause(0.8); seek(A); pause(0.8)   # select the first zizzle clip on V2, THEN seek (the click moves the playhead)
act('effects-browser'); pause(1.5); tab('Effects'); pause(1.5)

step('The Transform card', 'with a clip selected, the Effects page starts with its position, size and rotation')
snap('kf-card'); pause(2)

step('Pin a starting point', 'set the values, then the star (P) pins each one at the playhead')
for name, v in (('Width', 640), ('Height', 366), ('X', 380), ('Y', 260), ('Rotation', -12)):
    set_spin(name, v)
for name in ('X', 'Y', 'Width', 'Height', 'Rotation'):
    b = row_button(name, 'Pin')
    if b: click(*centre_of(b)); pause(0.4)
T.log(f"pinned status {status()!r}"); snap('kf-pinned')

step('Change it later', 'move the playhead and change the values: they are keyed there too')
seek(B)
for name, v in (('Width', 1120), ('Height', 640), ('X', 1260), ('Y', 560), ('Rotation', 0)):
    set_spin(name, v)
T.log(f"second key status {status()!r}"); snap('kf-second')

step('It moves', 'the picture glides in, grows and straightens between the two keyframes')
seek(A - 10); act('play-pause'); pause(4); act('play-pause'); pause(0.8); snap('kf-played')

step('Drag on the preview', 'on a keyframed picture, dragging it on the preview sets a keyframe at the playhead')
sel_v2(int((A + B) / 2))
for f in (40, 60, 80):
    seek(f); pause(0.5)
    T.log(f"frame {f}: X {spin('X').get_value_iface().get_current_value():.0f}")
sel_v2(int((A + B) / 2))
# Aim at the PiP's centre from the card's own values: the preview shows the
# 1920x1080 frame at (175, 52)-(1369, 716) with the inspector open (measured, 0.80).
X0, Y0, SX, SY = 175, 52, (1369 - 175) / 1920, (716 - 52) / 1080
vx = spin('X').get_value_iface().get_current_value(); vy = spin('Y').get_value_iface().get_current_value()
cx, cy = int(X0 + vx * SX), int(Y0 + vy * SY)
T.log(f"PiP centre {vx:.0f},{vy:.0f} -> screen {cx},{cy}")
drag(cx, cy, cx, cy - 150, dur=1.4); pause(1.5)
T.log(f"after drag X {spin('X').get_value_iface().get_current_value():.0f} Y {spin('Y').get_value_iface().get_current_value():.0f}")
T.log(f"drag status {status()!r}"); snap('kf-dragged')
seek(A - 10); act('play-pause'); pause(4); act('play-pause'); pause(0.8)

step('Curve lanes', 'press C: the transform\u2019s curves come first, one lane per value, a dot per keyframe')
sel_v2(A)
act('effects-curve-lanes'); pause(2.5); snap('kf-lanes'); pause(2.5)
act('effects-curve-lanes'); pause(1)

step('Touch-record', 'arm a value, play, and move it: U Stu keeps just enough keyframes to follow you')
sel_v2(A)
tr = row_button('Rotation', 'Touch-record')
if tr: click(*centre_of(tr)); pause(0.8)
seek(A); act('play-pause')
for v in (0, 8, 16, 8, -8, -16, -8, 0):
    set_spin('Rotation', v); pause(0.35)
act('play-pause'); pause(1.5)
if tr: click(*centre_of(tr)); pause(0.8)
T.log(f"record status {status()!r}")
seek(A); act('play-pause'); pause(4); act('play-pause'); pause(0.8); snap('kf-recorded')

step('(done)', '')
