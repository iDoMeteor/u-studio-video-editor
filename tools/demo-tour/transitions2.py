# Transitions 2 part: tiles with your own clips, zoom, spin, and the sound's crossfade. A fresh project (parts.json "script"); needs the effects
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
    return find(name, roles=('table cell',), exact=False, timeout=3)     # 'Dip to Black: Out to black…'
CUT_SEEK = 24                        # tens of frames: 0:08, two seconds before the cut
def seek_frame(f):
    # Frame steps queue up behind the engine; set the Seek slider (frames) directly.
    sl = find('Seek', roles=('slider',), timeout=3)
    if sl:
        sl.get_value_iface().set_current_value(float(f)); pause(1.0)
def play_across():
    seek_frame(8 * 30)                           # two seconds before the cut at 0:10
    act('play-pause'); pause(4.2); act('play-pause'); pause(0.8)
def try_style(name, snapname):
    t = tile(name)
    T.log(f"style {name}: {bool(t)}")
    if t:
        click(*centre_of(t)); pause(1.5); T.log(f"  status {status()!r}")
        play_across(); snap(snapname)

launch(); inspector(False)
import_folder(M + '/unicorn'); pause(6); rest()
click(1800, 752 + 38); pause(0.5)                   # V1 active
# A cut between two different clips, with spare frames: split the first clip
# at 0:10 and ripple-delete the rest of it, so it ends there with frames to
# spare right against the next clip (a split alone is the same shot on both sides).
act('seek-home'); act('step-forward-10', 30, gap=0.02); act('split-at-playhead'); pause(1.2)
act('zoom-fit'); pause(0.8)
click(170, 752 + 38); pause(0.8)                    # the right-hand piece (about 0:15)
act('ripple-delete-selected'); pause(1.5); rest()
T.log(f"trim status {status()!r}")
seek_frame(10 * 30 - 20)

step('Your own clips on the tiles', 'each tile now shows your two clips, half-way through that style')
act('effects-add-transition'); pause(4); T.log(f"T status {status()!r}")
for _ in range(4):
    mouse(1705, 600, 'b5c'); time.sleep(0.2)
pause(1.5)
for _ in range(4):
    mouse(1705, 600, 'b4c'); time.sleep(0.2)
pause(1); snap('tr2-tiles')

step('Zoom', 'the next clip grows in from the middle')
try_style('Zoom', 'tr2-zoom')

step('Spin', 'or grows in turning as it comes')
try_style('Spin', 'tr2-spin')

step('Sound: equal power', 'the sound can cross evenly, or at equal power, which keeps the level up through the middle')
dump('tr2-sound')
snd = None
for n in walk(app_node()):
    nm, ds, rl = info(n)
    if rl == 'combo box' and (nm.startswith('Even crossfade') or nm.startswith('Sound') or 'crossfade' in ds.lower() or 'Sound' in ds):
        snd = n
T.log(f"sound combo {info(snd) if snd else None}")
if snd:
    click(*centre_of(snd)); pause(1.2)
    e = find_any('Equal power', exact=True, timeout=2)
    if e: click(*centre_of(e)); pause(1.5)
    else: keysym(K_ESC)
T.log(f"sound status {status()!r}")
play_across(); snap('tr2-sound')

step('(done)', '')
