# GPU acceleration part: a three-track picture-in-picture layout played at
# Full with GPU acceleration on, then off, with the editor's processor use
# measured from /proc during each playback (tooling only, Linux-only by
# design: it measures the process it drives). Runs as TOUR_SCRIPT.
import os, sys, time
sys.path.insert(0, os.environ['TOUR_DEMO'])
import tourlib as T
from tourlib import *
M = os.environ['TOUR_MEDIA']
OUT = os.environ['TOUR_OUT']
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
last = None
def step(title, sub=''):
    global last
    if STOP_AFTER and last == STOP_AFTER:
        narration_wait(); pause(1.0); chapter('(end)'); quit_app(); sys.exit(0)
    last = title
    chapter(title, sub)

def cpu_seconds():
    """The editor's user+system CPU time so far (from /proc: tooling, Linux only)."""
    f = open(f'/proc/{T.proc.pid}/stat').read().rsplit(')', 1)[1].split()
    return (int(f[11]) + int(f[12])) / os.sysconf('SC_CLK_TCK')

def last_pause_frame():
    """The frame the engine last paused at (its debug log; the tour runs at debug)."""
    import re
    frames = re.findall(r'pause\(\) at frame (\d+)', open(os.path.join(OUT, 'app.stderr')).read())
    return int(frames[-1]) if frames else 0

def measured_play(secs):
    # Inside the first clips (the transforms are per clip; the zizzle clips are 6 s).
    act('seek-home'); act('step-forward', 10, gap=0.03); pause(1.5)
    c0, t0 = cpu_seconds(), time.time()
    act('play-pause'); pause(secs); act('play-pause')
    c1, t1 = cpu_seconds(), time.time()
    pause(0.5)
    fps = (last_pause_frame() - 10) / (t1 - t0)       # frames the playhead covered per second
    return 100.0 * (c1 - c0) / (t1 - t0), fps

def settings_performance():
    press('Settings'); pause(2)
    click(870, 786); pause(2.5)                       # the Performance tab (measured, 0.78)

def gpu_switch():
    for n in walk(app_node()):
        nm, ds, rl = info(n)
        if rl in ('switch', 'toggle button', 'check box') and nm.startswith('GPU acceleration'):
            return n
    return None

def set_scale(label):
    c = find('Auto (', roles=('combo box',), timeout=3) or find('Full', roles=('combo box',), timeout=1)
    if c:
        click(*centre_of(c)); pause(1.2); dump('scale-menu')
        item = find_any(label, exact=True, timeout=2)
        if item:
            click(*centre_of(item))
        else:
            keysym(K_ESC)
        pause(1)

launch(); inspector(False)
import_folder(M + '/unicorn'); pause(6); rest()
press('Add track'); pause(1.2); import_folder(M + '/zizzle'); pause(6); rest()
press('Add track'); pause(1.2); import_folder(M + '/zizzle'); pause(6); rest()

step('GPU acceleration', 'Settings › Performance › Hardware: the row says what it uses, or why it can’t')
settings_performance(); snap('gpu-settings'); dump('gpu-settings')
pause(3); keysym(K_ESC); pause(1)

step('Three tracks, scaled and moved', 'a full-frame background with two pictures on top, each moved and scaled')
act('zoom-fit'); act('seek-home'); act('step-forward', 10, gap=0.03); pause(1.2)
click(960, 385); pause(1.0)                           # V3's picture
b = preview_box(); T.log(f"V3 box {b}")
if b:
    x0, y0, x1, y1 = b
    mx, my = (x0 + x1) // 2, (y0 + y1) // 2
    drag(x0 + 4, y0 + 4, mx + 8, my + 8, dur=1.2); pause(1.2)             # V3 into the bottom-right quarter
    click(x0 + (x1 - x0) // 4, y0 + (y1 - y0) // 4); pause(1.0)           # now V2's picture, still full-frame
    b2 = preview_box(); T.log(f"V2 box {b2}")
    if b2:
        a0, c0, a1, c1 = b2
        drag(a0 + 4, c0 + 4, (a0 + a1) // 2 + 8, (c0 + c1) // 2 + 8, dur=1.2); pause(1.2)   # scale from its top-left
        b3 = preview_box(); T.log(f"V2 scaled {b3}")
        if b3:
            cx, cy = (b3[0] + b3[2]) // 2, (b3[1] + b3[3]) // 2
            drag(cx, cy, x0 + (x1 - x0) // 4 + 8, y0 + (y1 - y0) // 4 + 8, dur=1.2); pause(1.2)   # to the top-left
click(1850, 300); pause(0.8); rest(); snap('gpu-layout')

step('Playing on the graphics card', 'preview at Full: the graphics card places and scales every picture')
set_scale('Full'); snap('gpu-scale')
on, on_fps = measured_play(5); T.log(f"GPU on: CPU {on:.0f}%, {on_fps:.1f} fps"); snap('gpu-on-playing')

step('Turning it off', 'the same switch, for comparison: now the processor does all the work')
settings_performance()
sw = gpu_switch(); T.log(f"switch {info(sw) if sw else None}")
if sw:
    click(*centre_of(sw)); pause(2); snap('gpu-off-settings')
keysym(K_ESC); pause(1.5)

step('Playing on the processor', 'the same layout at Full, with GPU acceleration off')
off, off_fps = measured_play(5); T.log(f"GPU off: CPU {off:.0f}%, {off_fps:.1f} fps"); snap('gpu-off-playing')

json.dump({'on': round(on), 'off': round(off), 'on_fps': round(on_fps, 1), 'off_fps': round(off_fps, 1)}, open(os.path.join(OUT, 'gpu-cpu.json'), 'w'))
step('The difference', f'the editor’s processor use while playing: {on:.0f}% with the graphics card, {off:.0f}% without')
pause(6)
step('(done)', '')
