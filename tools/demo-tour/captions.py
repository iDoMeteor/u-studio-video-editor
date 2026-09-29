# Captions part: import an .srt made at runtime, play it, fix a word, export.
# A fresh project, not the long tour (parts.json "script"). Needs the titles
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

# The subtitle file: plain lines, a coloured one, italics, and one at the top.
os.makedirs(WORK, exist_ok=True)
SRT = os.path.join(WORK, 'unicorn-dj.srt')
cues = [
    (1.0, 4.0, 'Far from the city lights, a lonley unicorn plays for nobody.'),   # the typo is fixed on screen
    (4.5, 7.5, 'Every night, the same <font color="cyan">neon dream</font>.'),
    (8.0, 11.0, '<i>What if somebody is listening?</i>'),
    (11.5, 14.5, '{\\an8}♪ soft house music ♪'),
    (15.0, 18.5, 'One more track.\nOne more chance.'),
    (19.0, 22.0, 'And then the crowd arrives.'),
]
def srt_time(t):
    ms = int(round(t * 1000)); h, ms = divmod(ms, 3600000); m, ms = divmod(ms, 60000); s, ms = divmod(ms, 1000)
    return f'{h:02d}:{m:02d}:{s:02d},{ms:03d}'
with open(SRT, 'w', encoding='utf-8') as f:
    for i, (a, b, text) in enumerate(cues, 1):
        f.write(f'{i}\n{srt_time(a)} --> {srt_time(b)}\n{text}\n\n')

launch(); inspector(False)
import_folder(M + '/unicorn'); pause(6); rest()
act('seek-home'); pause(0.5)

step('Import captions', 'Import an .srt or .vtt file: every line becomes a caption, on a new Captions track at the top')
import_file(SRT); pause(3); T.log(f"captions status {status()!r}"); rest(); snap('cap-imported'); dump('cap-imported')

step('Captions play over the video', 'bold, italic and colours from the file come through; captions the file puts at the top go at the top')
act('seek-home'); act('play-pause'); pause(12); snap('cap-playing'); pause(3); act('play-pause'); pause(0.8)

step('Fix a caption', 'select a caption and correct its words on the Title page; the timeline names it by its first line')
click(1800, 752 + 38); pause(0.6)             # the Captions track (top row) becomes the active track
act('seek-home'); act('select-next-clip'); T.log(f"select caption: {status()!r}")
act('zoom-in', 5, gap=0.25); pause(1)
inspector(True); pause(1.2)
field = find('caption', roles=('text',), exact=True, timeout=3)
if field:
    click(*centre_of(field)); pause(0.4); keysym(ord('a'), mods=('ctrl',)); pause(0.3)
    typestr('Far from the city lights, a lonely unicorn plays for nobody.'); pause(1.5)
dump('cap-inspector'); snap('cap-fixed')
act('seek-home'); act('play-pause'); pause(2.6); act('play-pause'); pause(1.2); snap('cap-fixed-preview')   # the fixed line on screen

step('Export captions', 'Export Captions… writes every caption, fixes included, as .srt or .vtt')
press('Export Captions…'); center_dialogs(1.2); snap('cap-export'); dump('cap-export')
VTT = os.path.join(WORK, 'unicorn-dj-fixed.vtt')
for n in walk(app_node()):
    nm, ds, rl = info(n)
    if rl in ('text', 'entry') and nm.startswith('Name'):
        n.get_editable_text_iface().set_text_contents(VTT); break
pause(1.2); press('Save', exact=True); pause(2)
T.log(f"export status {status()!r}; exists {os.path.exists(VTT)}")
if os.path.exists(VTT):
    T.log('vtt head: ' + open(VTT, encoding='utf-8').read()[:200].replace('\n', ' | '))
snap('cap-exported')

step('(done)', '')
