# Titles 4 part: animated layers (Lottie) in U Stu Titles: the ringing-bell
# template, + > Animation…, Plays and Speed, and the result in the editor.
# A fresh project (parts.json "script"); needs the titles drop-in. Runs as TOUR_SCRIPT under run_tour.sh.
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



TG = ('toggle button',)
def seek(f):
    sl = find('Seek', roles=('slider',), timeout=3)
    if sl: sl.get_value_iface().set_current_value(float(f)); pause(0.8)
def titles_window():
    use_app('u-studio-titles')
    for _ in range(40):
        if app_node(): break
        time.sleep(0.5)
    subprocess.run(['python3', os.path.join(T.DEMO, 'fitwin.py'), 'u-studio-titles'], capture_output=True, timeout=30)
    pause(1.5)
def canvas_at():
    cv = find('Title canvas', roles=('grouping', 'panel', 'drawing area', 'canvas', 'filler'), timeout=3)
    x, y, w, h = extents(cv); x += FRAME_OFF[0]; y += FRAME_OFF[1]
    sc = min((w - 48) / 1920, (h - 48) / 1080); ox = x + (w - 1920 * sc) / 2; oy = y + (h - 1080 * sc) / 2
    return lambda cx, cy: (int(ox + cx * sc), int(oy + cy * sc))

import subprocess
WORK = os.path.join(OUT, 'work'); os.makedirs(WORK, exist_ok=True)
LOTTIE = os.path.join(WORK, 'unicorn-star.json')
subprocess.run(['python3', os.path.join(T.DEMO, 'make_lottie.py'), LOTTIE], check=True)

launch(); inspector(False)
import_folder(M + '/unicorn'); pause(6); rest()
press('Add track'); pause(1.2)
click(1500, 752 + 38); pause(0.6)                 # the new top track is active
seek(45); pause(0.5)

step('Animated templates', 'two built-in templates have a ringing bell: an animated layer inside the title')
act('titles-new'); pause(5)
titles_window()
press('Lower third with ringing bell', exact=True); pause(3)
press('Play the intro'); pause(6); snap('an-bell'); press('Play the intro'); pause(0.6)

step('Add an animation', '+ › Animation…: pick a Lottie (.json) file; it plays inside the title, sharp at any size')
at = canvas_at()
click(*at(1700, 150)); pause(0.5)                 # canvas focus, nothing selected
press('Add a layer', roles=TG); pause(1.2); snap('an-menu')
# The popover's items aren't in the AT-SPI tree and it takes no keys on a
# WM-less Xvfb: click "Animation…", the 7th item (measured, 0.79).
click(60, 271); center_dialogs(1.2)
set_location(LOTTIE); keysym(K_RETURN); pause(1.2)
if find('Open', exact=True, timeout=1):
    press('Open', exact=True)
pause(2.5); snap('an-added'); dump('an-added')
drag(*at(960, 540), *at(1560, 330), dur=1.4); pause(1.5)
press('Play the intro'); pause(5); snap('an-playing'); press('Play the intro'); pause(0.6)

step('Plays and Speed', 'play it over and over, or once and hold the last frame; and faster or slower, 25% to 400%')
dump('an-inspector')
plays = find('Plays', roles=('combo box',), timeout=3)
if plays:
    click(*centre_of(plays)); pause(1.2)
    o = find_any('Once, then holds', exact=True, timeout=2)
    if o: click(*centre_of(o)); pause(1.2)
    else: keysym(K_ESC)
# "Speed (%)" is a spin button in the AT-SPI tree since 0.80.1 (VE Text).
sp = find('Speed (%)', roles=('spin button',), exact=True, timeout=3)
if sp:
    sp.get_value_iface().set_current_value(200.0); pause(1.2)
else:                                  # older builds: its + button, measured, 4 × 25%
    for _ in range(4):
        click(1876, 244); pause(0.4)
T.log(f"plays {bool(plays)} speed spin {bool(sp)}")
press('Play the intro'); pause(5); snap('an-once'); press('Play the intro'); pause(0.6)

step('In the editor', 'save, and the animation plays in the editor and in exports, frame for frame')
keysym(ord('s'), mods=('ctrl',)); pause(2)
press('Close', exact=True); pause(2)
use_app(None)
seek(45 + 30); act('play-pause'); pause(1.5); snap('an-editor'); pause(2.5); act('play-pause'); pause(1)

step('(done)', '')
