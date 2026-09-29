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
act('seek-home'); act('step-forward-10', 6, gap=0.05); pause(1)
click(120, 752 + 38); pause(0.8)                  # select the first clip

step('The Add page', 'select a clip and press E: a tile for every effect, each showing your own frame with it')
act('effects-browser'); pause(3)
for _ in range(6):
    mouse(1705, 600, 'b5c'); time.sleep(0.15)
pause(1.5)
for _ in range(6):
    mouse(1705, 600, 'b4c'); time.sleep(0.15)
pause(1); snap('fx1-add')

step('Search and try', 'type to search; point at a tile and the picture shows your clip with that effect, nothing changed yet')
s = find('Search effects', roles=('entry', 'text'), timeout=2)
if s:
    click(*centre_of(s)); pause(0.4); typestr('glow'); pause(2.5)
for name in ('Soft Glow', 'Glow'):
    t = tile(name)
    if t:
        move(*centre_of(t), dur=0.8); pause(3); snap(f'fx1-try-{name.split()[0].lower()}')

step('Add an effect', 'click the tile to add it')
t = tile('Glow')
if t:
    click(*centre_of(t)); pause(2.5)
T.log(f"add status {status()!r}"); snap('fx1-added')

step('The Effects page', 'what is on the clip, with every setting, and Mix to blend it with the original picture')
tab('Effects'); pause(2); dump('fx1-rack'); snap('fx1-rack')

step('Looks', 'ready-made sets of effects: the brand looks come with U Stu, and you can save your own')
tab('Add')
s = find('Search effects', roles=('entry', 'text'), timeout=2)
if s:
    click(*centre_of(s)); pause(0.3); keysym(ord('a'), mods=('ctrl',)); typestr('neon'); pause(2.5)
dump('fx1-looks')
t = tile('Neon Night')
if t:
    move(*centre_of(t), dur=0.8); pause(3); snap('fx1-look-try')
    click(*centre_of(t)); pause(2.5)
T.log(f"look status {status()!r}"); snap('fx1-look')

step('Before and after', 'Compare splits the picture: before on the left, after on the right')
act('effects-compare'); pause(2)
act('play-pause'); pause(4); act('play-pause'); pause(1); snap('fx1-compare')
act('effects-compare'); pause(1)

step('(done)', '')
