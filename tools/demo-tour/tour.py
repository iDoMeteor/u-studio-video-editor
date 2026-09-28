# U Stu Video Editor demo tour. Chapters run in order; TOUR_UPTO stops early for testing.
import os, sys, time
sys.path.insert(0, os.environ['TOUR_DEMO'])
import tourlib as T
from tourlib import *
M = os.environ['TOUR_MEDIA']
WORK = os.path.join(os.environ['TOUR_OUT'], 'work')
UPTO = int(os.environ.get('TOUR_UPTO', '99'))
SHOTS = os.environ.get('TOUR_SHOTS', '1') == '1'
TL_X0 = 36          # the timeline's time zero, in screen x

def seek_x(x):
    """Move the playhead under timeline x (zoom-fit). The ruler takes no clicks,
    so set the transport's Seek slider through AT-SPI."""
    ends = [r[1] for row in range(4) for r in clips_in_row(752 + 60 * row + 38)]
    end = max(ends) if ends else 1735
    sl = find('Seek', roles=('slider',), timeout=3)
    if not sl:
        T.log('seek_x: no Seek slider'); return False
    vi = sl.get_value_iface()
    lo, hi = vi.get_minimum_value(), vi.get_maximum_value()
    v = lo + (hi - lo) * max(0.0, min(1.0, (x - TL_X0) / (end - TL_X0)))
    vi.set_current_value(v); T.log(f"seek_x {x}: {v:.1f} of {lo:.0f}..{hi:.0f}")
    return True

# A 3-second numbered PNG run for the image-sequence chapter, made from a staged clip.
import subprocess
SEQ_DIR = os.path.join(WORK, 'sequence')
os.makedirs(SEQ_DIR, exist_ok=True)
subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-ss', '1', '-t', '3', '-i', os.path.join(M, 'zizzle', 'chunk_005.mp4'),
                '-vf', 'fps=30,scale=960:-2', os.path.join(SEQ_DIR, 'frame_%04d.png')], check=True)
SEQ_FIRST = os.path.join(SEQ_DIR, 'frame_0001.png')
def snap(n):
    if SHOTS: shot(n)

ROW0 = 752          # top of the first track row (60 px rows)
def row_y(r, part='body'):
    return ROW0 + r * 60 + (38 if part == 'body' else 22)

def import_folder(folder):
    act('import'); center_dialogs(1.0)
    set_location(folder.rstrip('/') + '/'); keysym(K_RETURN); pause(1.5)
    o = dialog_origin()
    if not o:
        snap('ERR-no-dialog'); raise SystemExit('import dialog vanished')
    (dx, dy), dw, dh = o
    click(dx + 300, dy + 93)
    keysym(ord('a'), mods=('ctrl',)); pause(0.8)
    press('Open', exact=True)

def import_files(paths):
    act('import'); center_dialogs(1.0)
    typestr(paths[0]); pause(0.6); keysym(K_RETURN)

def boxes(r):
    return clips_in_row(row_y(r))

def hover(x, y, wait=1.2):
    move(x, y, dur=0.5); pause(wait)

# =====================================================================
n = 0
STOP_AFTER = os.environ.get('TOUR_STOP_AFTER', '')   # a part's last chapter (make_demo.sh, parts.json)
last_title = None
def step(title, sub=''):
    global n, last_title
    n += 1
    if n > UPTO or (STOP_AFTER and last_title == STOP_AFTER):
        narration_wait(); pause(1.0)
        chapter('(end)'); quit_app(); sys.exit(0)
    last_title = title
    chapter(title, sub)

# TOUR_GPU=0 presets GPU acceleration off in the run's private dconf (dconf
# honours XDG_CONFIG_HOME, so the owner's settings are untouched).
if os.environ.get('TOUR_GPU', '1') == '0':
    subprocess.run(['dconf', 'write', '/com/ustudio/VideoEditor/gpu-acceleration', 'false'], check=True)
    T.log('GPU acceleration preset off')
launch()
step('U Stu Video Editor', 'a GNOME-native multi-track video editor')
pause(3)

# ---------------------------------------------------------------- 1
step('Import', 'pick several files: they land in order on the active track, and the project takes its format from the first')
import_folder(M + '/unicorn')
pause(6); rest(); snap('c1-imported')
step('Clip tooltips', 'rest the pointer on a clip: the frame under it, its timecode and details')
hover(520, row_y(0), 3.0); snap('c1-tooltip')
rest()

# ---------------------------------------------------------------- 2
step('Mixed frame rates', 'a 25 fps 1344×768 set joins a 30 fps project, and every frame lands at its time')
press('Add track'); pause(1.5)
import_folder(M + '/zizzle')
pause(6); rest(); snap('c2-mixed')

# ---------------------------------------------------------------- 3
step('Import a folder', 'Import Folder (Ctrl+Shift+I) fills the bin; one undo step, and failures are listed')
act('import-folder'); center_dialogs(1.0)
set_location(M + '/stills/'); pause(0.5)
if not (press('Select', exact=True) or press('Open', exact=True)):
    keysym(K_RETURN)
pause(3); rest()
step('Media browser', 'every asset with its length, frame rate and format; drag one onto the timeline')
press('Show or hide the media browser'); pause(2)
for _ in range(12):
    mouse(230, 400, 'b5c'); time.sleep(0.08)
pause(1.2); snap('c3-browser')
v2 = [r for r in clips_in_row(752 + 38)]
v2_end = max(r[1] for r in v2) if v2 else 900
drag(70, 660, max(v2_end + 60, 520), 752 + 38, dur=1.6); pause(2); rest(); snap('c3-dropped')
step('Proxies', 'right-click an asset: Create Proxy makes a light editing copy in the background; badges show kind, size and state')
for _ in range(14):
    mouse(230, 300, 'b4c'); time.sleep(0.08)
pause(1); click(230, 92, button=3); pause(1.2); press('Create Proxy'); pause(9); rest(); snap('c3-proxy')
press('Proxies'); pause(1.5); T.log(f"proxies toggle status {status()!r}")
press('Show or hide the media browser'); pause(1)

# ---------------------------------------------------------------- 3b soundtrack via Split Audio
step('Split audio', "bring in the finished unicorn-DJ cut and peel its soundtrack onto an audio track")
press('Add track'); pause(1.2)
act('import'); center_dialogs(1.0); set_location(M + '/finals/unicorn-dj-final.mp4'); keysym(K_RETURN); pause(6); rest()
click(300, 790, button=3); pause(1.2); press('Split Audio'); pause(2.5); rest(); snap('c3b-split')
click(300, 790); pause(0.5); act('delete-selected-clip'); pause(1.5)
step('Trim to fit', 'drag the soundtrack’s end back; it snaps to the end of the edit')
ends = clips_in_row(752 + 120 + 38)
v1_end = max(r[1] for r in ends) if ends else 830
a1 = [r for r in clips_in_row(752 + 180 + 38) if r[1] - r[0] > 40]
a1_end = max(r[1] for r in a1) if a1 else 1735
T.log(f"v1_end {v1_end} a1_end {a1_end}")
click(a1_end - 200, 752 + 180 + 38); pause(0.4)
drag(a1_end - 2, 752 + 180 + 38, v1_end + 2, 752 + 180 + 38, dur=1.6); pause(1); rest()
act('zoom-fit'); pause(1.2); snap('c3b-trimmed')

# ---------------------------------------------------------------- 4
step('Playback', 'real-time playback with sound; the editor stays responsive')
act('seek-home'); act('play-pause'); pause(5); act('play-pause'); pause(0.8)
step('Shuttle and step', 'J / K / L shuttle, frame steps, 10-frame steps')
act('shuttle-forward', 2, gap=1.2); pause(1.5); act('shuttle-stop'); pause(0.6)
act('shuttle-reverse'); pause(1.5); act('shuttle-stop'); pause(0.6)
act('step-forward', 5, gap=0.25); act('step-backward', 3, gap=0.25); act('step-forward-10', 3, gap=0.3)
step('Loop a range', 'set loop in and out at the playhead, then play it round')
act('seek-home'); act('step-forward-10', 6, gap=0.1); act('loop-set-in'); act('step-forward-10', 9, gap=0.1); act('loop-set-out')
act('play-pause'); pause(6); act('play-pause'); press('Clear loop'); pause(0.8)
snap('c4-transport')

# ---------------------------------------------------------------- 5 editing
# Rows after adding V3 on top: row0 V3 (empty), row1 V2 (zizzle), row2 V1 (unicorn).
RT = lambda r: 752 + 60 * r
YB = lambda r: RT(r) + 38
def sel(x, r):
    click(x, YB(r)); pause(0.5)
    b = selected_box(RT(r), x); T.log(f"sel r{r} x{x}: {b}")
    return b
def try_edit(fn, undo=False, name=''):
    before = status(); fn(); pause(1.2); rest(); pause(0.4)
    ok = edited(before); T.log(f"{name}: ok={ok} status={status()!r}")
    if undo and ok:
        act('undo'); pause(0.8)
    return ok

step('Edit on the timeline', 'working on the unicorn clips on V1, with the empty V3 on top')
act('zoom-fit'); pause(0.6); act('seek-home'); act('zoom-in', 2, gap=0.6); pause(1)

step('Move', 'drag a clip anywhere, even onto another track')
b = sel(760, 2)
try_edit(lambda: drag((b[0] + b[1]) // 2, YB(2), (b[0] + b[1]) // 2 + 150, YB(0), dur=1.2), undo=True, name='move')
snap('c5-move')

step('Trim', 'drag an edge; the gap it leaves is yours to fill')
b = sel(760, 2)
try_edit(lambda: drag(b[1] - 2, YB(2), b[1] - 140, YB(2), dur=1.0), undo=True, name='trim')

step('Ripple trim (Alt+drag)', 'later clips follow the edge, so no gap opens')
b = sel(760, 2)
try_edit(lambda: drag(b[1] - 2, YB(2), b[1] - 120, YB(2), dur=1.0, mods=('alt',)), name='ripple trim')
snap('c5-ripple-trim')

step('Slip (Shift+drag)', 'same place and length, different source frames')
b = sel(720, 2)
try_edit(lambda: drag(b[0] + 3, YB(2), b[0] - 60, YB(2), dur=1.2, mods=('shift',)), name='slip')

step('Dissolve', 'right-click a cut: Add Transition')
b = sel(720, 2)
click(b[1] + 1, YB(2), button=3); pause(1.2); press('Add Transition'); pause(1.2); rest()
T.log(f"dissolve status {status()!r}"); snap('c5-dissolve')
click(b[1] - 80, YB(0)); pause(0.3)            # scrub to just before the cut
act('play-pause'); pause(4); act('play-pause'); pause(0.8)

step('Copy (Ctrl+drag)', 'drop a copy onto the empty track')
b = sel(250, 2)
try_edit(lambda: drag((b[0] + b[1]) // 2, YB(2), (b[0] + b[1]) // 2 + 40, YB(0), dur=1.2, mods=('ctrl',)), name='copy')
snap('c5-copy')

step('Nudge', 'comma and period move the selection a frame; with Shift, ten')
b = sel(300, 0)
act('nudge-right', 6, gap=0.2); act('nudge-right-10', 4, gap=0.3); rest(); pause(1); T.log(f"nudge {status()!r}")

step('Split and ripple delete', 'X splits at the playhead; Shift+Delete removes it and closes the gap')
b = sel(1300, 2)
act('split-at-playhead'); pause(1.2); rest(); T.log(f"split {status()!r}"); snap('c5-split')
b = sel(1340, 2)
act('ripple-delete-selected'); pause(1.5); rest(); T.log(f"ripple delete {status()!r}"); snap('c5-ripple-delete')

step('Markers', 'M drops a marker at the playhead')
click(1500, YB(0)); act('add-marker'); pause(0.5)
click(1750, YB(0)); act('add-marker'); pause(0.8); rest()

step('Rubber band select (Shift+drag)', 'every clip the band touches joins the selection')
drag(80, 975, 900, RT(1) + 5, dur=1.2, mods=('shift',)); pause(1); rest(); snap('c5-band')
act('clear-selection'); pause(0.5)

step('Ripple mode', 'R turns it on: moves close their gap and push later clips along')
press('Ripple'); pause(1)
b = sel(1250, 2)
try_edit(lambda: drag((b[0] + b[1]) // 2, YB(2), 90, YB(2), dur=1.4), name='ripple move')
snap('c5-ripple-move')
press('Ripple'); pause(0.6)

step('Undo and redo', 'every edit is one step: Ctrl+Z and Shift+Ctrl+Z')
for _ in range(3):
    press('Undo'); pause(0.7)
for _ in range(3):
    press('Redo'); pause(0.7)
act('zoom-fit'); pause(1); rest(); snap('c5-end')

# ---------------------------------------------------------------- 5b transform
step('Transform on the preview', 'click a picture to select it; drag a corner to scale, the body to move; guides snap')
act('zoom-fit'); act('seek-home'); act('step-forward-10', 6, gap=0.1); pause(1.5)
click(960, 385); pause(1.2)
b = preview_box(); T.log(f"preview box {b}")
if b:
    x0, y0, x1, y1 = b
    drag(x0 + 1, y0 + 1, x0 + (x1 - x0) // 2, y0 + (y1 - y0) // 2, dur=1.4); pause(1.2)
    b = preview_box() or b; x0, y0, x1, y1 = b
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    drag(cx, cy, cx + 40, cy - 60, dur=1.4); pause(1.2); rest(); snap('c5b-pip')
    step('Rotate', 'drag the knob above the box; Shift steps by 15°')
    k = preview_knob(); T.log(f"knob {k}")
    if k:
        drag(k[0], k[1], k[0] + 70, k[1] + 12, dur=1.4); pause(1.2); rest(); snap('c5b-rotated')
    step('Transform menu', 'right-click the preview: fit, stretch, centre, flip and rotate')
    b = preview_box() or b; x0, y0, x1, y1 = b
    click((x0 + x1) // 2, (y0 + y1) // 2, button=3); pause(1.5); snap('c5b-menu')
    # The menu's items aren't in the AT-SPI tree under Xvfb; close it and flip
    # through the same action the item runs.
    keysym(K_ESC); pause(0.5); act('transform-flip-h'); pause(1.2); rest(); snap('c5b-flipped')
step('Edit Transform', 'Ctrl+T shows exact values beside the preview; every change is one undo step')
act('transform-edit'); pause(3); snap('c5b-dialog')
# Its own window, and Esc only reaches it with focus, which Xvfb doesn't give.
press_in('Edit Transform', 'Close'); pause(0.8)
act('play-pause'); pause(4); act('play-pause'); pause(0.8)

# ---------------------------------------------------------------- 5c image sequence
# Numbered frames generated at runtime from a staged clip; nothing goes in the repo.
step('Import Image Sequence', 'Ctrl+Alt+I: pick any image of a numbered run and it comes in as one clip, one image per frame')
act('import-image-sequence'); center_dialogs(1.0)
set_location(SEQ_FIRST); keysym(K_RETURN); pause(1.2)
if find('Open', exact=True, timeout=1):   # Enter may only navigate
    press('Open', exact=True)
pause(3); T.log(f"sequence status {status()!r}")
press('Show or hide the media browser'); pause(1.5)
for _ in range(14):
    mouse(230, 400, 'b5c'); time.sleep(0.08)
pause(1.2); snap('c5c-browser')
v3 = clips_in_row(YB(0))
v3_end = max(r[1] for r in v3) if v3 else 200
drop_x = v3_end + 40
drag(70, 695, drop_x, YB(0), dur=1.6); pause(2); rest(); snap('c5c-dropped')   # the newest asset is the last row
press('Show or hide the media browser'); pause(1)
seek_x(drop_x + 8); pause(0.8)   # playhead onto the sequence
act('zoom-in', 4, gap=0.3); pause(1)
act('play-pause'); pause(1.5); snap('c5c-playing'); pause(1.2); act('play-pause'); pause(0.8)
act('undo'); pause(1); act('zoom-fit'); rest()   # keep the later chapters' timeline as it was

# ---------------------------------------------------------------- 5d titles (U Stu Titles)
def wait_status(prefixes, timeout):
    end = time.time() + timeout
    while time.time() < end:
        st = status()
        if st.startswith(prefixes):
            return st
        time.sleep(0.5)
    return status()

TG = ('toggle button',)
def dump(name):
    if SHOTS: dump_tree(os.path.join(os.environ['TOUR_OUT'], f'tree-{name}.txt'))
def titles_window():
    """Point the helpers at U Stu Titles once it's up, and size its window."""
    use_app('u-studio-titles')
    for _ in range(40):
        if app_node(): break
        time.sleep(0.5)
    subprocess.run(['python3', os.path.join(T.DEMO, 'fitwin.py'), 'u-studio-titles'], capture_output=True, timeout=30)
    pause(1.5)
PACK = os.path.join(WORK, 'stream-kit.zip')
subprocess.run(['python3', os.path.join(T.DEMO, 'make_demo_pack.py'), PACK], check=True)

step('Titles', 'New Title (Shift+T): a title clip at the playhead on the active track, and U Stu Titles opens on it')
v3 = clips_in_row(YB(0))
title_x = (max(r[1] for r in v3) if v3 else 300) + 60
click(1500, YB(0)); pause(0.6)                 # the empty end of V3: make it the active track
seek_x(title_x); pause(0.8)
act('titles-new'); pause(5)
titles_window(); snap('c5d-gallery')
step('Template gallery', 'lower thirds, bugs and badges, cards, end screens, countdowns, live and social, and your own')
for _ in range(10):
    mouse(960, 600, 'b5c'); time.sleep(0.12)
pause(1.5)
for _ in range(10):
    mouse(960, 600, 'b4c'); time.sleep(0.12)
pause(1)
step('Template packs', 'Open Pack… shows what a shared pack holds before installing it; Save as Pack… makes one')
press('Open Pack…'); pause(1.5); set_location(PACK); keysym(K_RETURN); pause(1.2)
if find('Open', exact=True, timeout=1):
    press('Open', exact=True)
pause(2.5); snap('c5d-pack')
press('Install', exact=True); pause(2.5)
for _ in range(40):
    mouse(960, 600, 'b5c'); time.sleep(0.06)
pause(2); snap('c5d-pack-installed')
for _ in range(40):
    mouse(960, 600, 'b4c'); time.sleep(0.06)
pause(1)
press('Lower third, two lines', exact=True); pause(3); snap('c5d-template')
step('Designing over the picture', 'the video at the playhead shows behind the title: layers on the left, the inspector on the right')
layer = find('{{name}}', roles=('label',), timeout=3)
if layer:
    click(*centre_of(layer)); pause(1.5)
dump('c5d-layer'); snap('c5d-layer')
step('Brand kit', 'the Unicorn Tears colours, gradients and fonts sit next to every colour and font; Apply Brand restyles a title')
# The brand fonts aren't installed here (they fall back), so point at the kit
# rather than click a font that would look unchanged.
a = find('Anton', roles=('button',), exact=True, timeout=2)
if a:
    ax, ay = centre_of(a)
    move(ax, ay - 40); pause(0.8); move(ax, ay + 110, dur=1.2); pause(1)
b = where('Apply Brand')
if b:
    move(*b, dur=1.0); pause(2)
snap('c5d-brand')
step('Animation', 'Add In…, Out… and Loop… show each behaviour on your own layer; the strip shows intro, hold and outro')
press('Add In…', roles=TG, exact=True); pause(3)
p = find('Pop', roles=('table cell',), exact=True, timeout=2)
if p:
    click(*centre_of(p)); pause(1.2)
else:
    keysym(K_ESC); pause(0.6)
press('Add Loop…', roles=TG, exact=True); pause(2.5)
p = find('Glow breathe', roles=('table cell',), exact=True, timeout=2)
if p:
    click(*centre_of(p)); pause(1.2)
else:
    keysym(K_ESC); pause(0.6)
press('Play the intro'); pause(5); snap('c5d-playing'); press('Play the intro'); pause(0.6)
step('Export for OBS', 'Ctrl+E: ProRes 4444, WebM or a PNG sequence with transparency, or H.264, at any length')
keysym(ord('e'), mods=('ctrl',)); pause(2); snap('c5d-export'); dump('c5d-export')
keysym(K_ESC); pause(1)
step('Save the title', 'Ctrl+S: the editor picks it up at once, with clean transparent edges over the video')
keysym(ord('s'), mods=('ctrl',)); pause(2)
press('Close', exact=True); pause(2)
use_app(None)
act('zoom-in', 4, gap=0.3); pause(1)
# The playhead is at the title's first frame: step back one so the title is
# the "next" clip on the active track, select it, then go into the hold.
act('step-backward'); act('select-next-clip'); T.log(f"select title: {status()!r}")
act('step-forward-10', 5, gap=0.2); pause(1.5); snap('c5d-editor')
step('Title fields', 'the inspector’s Title page: one lower third for every guest, and the preview follows as you type')
click(*where('Inspector', roles=TG)); pause(2); dump('c5d-titlepage'); snap('c5d-titlepage')
fields = [n for n in walk(app_node()) if info(n)[2] in ('text', 'entry') and extents(n)[0] > 1400]
T.log(f"title fields: {[info(f)[0] for f in fields]}")
for f, text in zip(fields, ('DJ Unicorn', 'Resident, Neon Grove')):
    click(*centre_of(f)); pause(0.4); keysym(ord('a'), mods=('ctrl',)); typestr(text); pause(1.2)
pause(1.5); snap('c5d-fields')
step('Bake', 'Bake… renders the title to a ProRes 4444 file and plays that instead; Ctrl+Z brings the live title back')
if press('Bake…'):
    ff_start(); st = wait_status(('Baked', "Couldn't bake"), 180); ff_end(); T.log(f"bake status {st!r}")
pause(2); snap('c5d-baked')
act('undo'); pause(1.5)
insp = find('Inspector', roles=TG, timeout=2)
if insp and insp.get_state_set().contains(Atspi.StateType.CHECKED):
    click(*centre_of(insp)); pause(1)      # close it only if it's open
act('zoom-fit'); pause(1)

# ---------------------------------------------------------------- 6 save
PROJECT = os.path.join(WORK, 'unicorn-demo.ustudio')
HDR = {'save': where('Save project'), 'render': where('Render…')}   # the header changed in 0.67; find, don't guess
def name_entry_set(path):
    for n in walk(app_node()):
        nm, ds, rl = info(n)
        if rl in ('text', 'entry') and nm.startswith('Name'):
            n.get_editable_text_iface().set_text_contents(path); return True
    return False

import shutil
os.makedirs(os.path.join(WORK, 'footage'), exist_ok=True); os.makedirs(os.path.join(WORK, 'archive'), exist_ok=True)
EXTRA = os.path.join(WORK, 'footage', 'zizzle-extra.mp4')
shutil.copy(os.path.join(M, 'zizzle', 'chunk_003.mp4'), EXTRA)
act('import'); center_dialogs(1.0); set_location(EXTRA); keysym(K_RETURN); pause(4); rest()
act('zoom-fit'); act('seek-home'); act('step-forward-10', 45, gap=0.05); pause(1)   # a picture in the preview
step('Save', 'an untitled project asks where; after that Save writes in place')
press('Save project'); center_dialogs(1.2)
T.log(f"name entry: {name_entry_set(PROJECT)}"); pause(1.2)
press('Save', exact=True); pause(2); T.log(f"save status {status()!r}")
step('Automatic backups', 'every Save keeps the previous version in .ustudio-backups (newest 5)')
act('add-marker'); pause(0.6)
press('Save project'); pause(1.5); T.log(f"resave status {status()!r}")
step('Save As', 'right-click the Save button')
click(*HDR['save'], button=3); center_dialogs(1.2); snap('c6-saveas'); pause(1.5)
press('Cancel', exact=True); pause(0.8)

# ---------------------------------------------------------------- 7 settings
TABS = {'General': 663, 'Toggles': 763, 'Performance': 861, 'Locations': 960, 'Render': 1059, 'Drop-ins': 1157, 'Shortcuts': 1255}
step('Settings', 'every preference, grouped')
press('Settings'); pause(2)
for tab, sub in [('Toggles', 'reopen the last project, snapping, follow playhead, thumbnails, waveforms'),
                 ('Performance', 'preview scale, proxies, GPU acceleration and hardware video decoding, worker threads'),
                 ('Locations', 'default project and export folders'),
                 ('Render', 'render profiles (High quality is the default) and render threads'),
                 ('Drop-ins', 'optional effects, titles and more, installed separately'),
                 ('Shortcuts', 'custom keys are coming; Help lists every shortcut today')]:
    step(f'Settings: {tab}', sub)
    click(TABS[tab], 776); pause(3.2); snap(f'c7-{tab.lower()}')
keysym(K_ESC); pause(1)

# ---------------------------------------------------------------- 8 project frame rate
step('Project format', 'click the format under the title: frame rate and background colour, each one undo step')
press('unicorn-demo'); pause(1.5); snap('c8-dialog'); dump('c8-dialog')
# The Background row's colour chooser is its own window, which doesn't come up
# on the WM-less Xvfb; the dialog's row is shown, not driven.
cb = find('The colour wherever', roles=('button',), timeout=2)
if cb:
    move(*centre_of(cb)); pause(2.5)
click(960, 566); pause(1.2); snap('c8-list')
keysym(0xff52); pause(0.4); keysym(0xff52); pause(0.4); keysym(K_RETURN); pause(1)
press('Apply', exact=True); pause(2.5); T.log(f"fps status {status()!r}"); snap('c8-changed')
act('undo'); pause(1.5)

# ---------------------------------------------------------------- 9 render

step('Render', 'auto-named output in the export folder; High quality is the default profile')
press('Render…'); center_dialogs(1.2); snap('c9-dialog'); pause(2)
press('Save', exact=True); pause(4); snap('c9-rendering')
step('Queue another render', 'right-click the Render button for quick profiles')
click(*HDR['render'], button=3); pause(1.3); snap('c9-menu')
press_any('Queue “Draft'); pause(2); rest(); snap('c9-queued')
step('Cancel or queue', 'click the Render button while it renders')
click(*HDR['render']); pause(1.5); snap('c9-cancel-dialog')
press('Cancel Render', exact=True); pause(2)
step('Render finished', 'the button turns cyan; click it to open the video')
ff_start(); st = wait_status(('Rendered',), 900); ff_end(); T.log(f"render status {st!r}"); rest(); pause(2); snap('c9-done')
if not st.startswith('Rendered'):
    snap('ERR-render-unfinished'); raise SystemExit(f'render did not finish: {st!r}')
click(*HDR['render']); pause(4); snap('c9-player')
# close whatever player GIO launched on our Xvfb
import subprocess as _sp
for pid in os.listdir('/proc'):
    if not pid.isdigit() or int(pid) in (os.getpid(), T.proc.pid): continue
    try:
        env = open(f'/proc/{pid}/environ', 'rb').read().split(b'\0')
        cmd = open(f'/proc/{pid}/cmdline', 'rb').read().replace(b'\0', b' ').decode(errors='replace')
    except Exception:
        continue
    if ('DISPLAY=' + os.environ['DISPLAY']).encode() in env and b'TOUR_OUT=' not in env and not any(k in cmd for k in ('Xvfb', 'at-spi', 'dbus', 'ffmpeg', 'audiorec', 'python3', 'u-studio-video-editor', 'xdg-', 'portal', 'goa', 'gvfs', 'dconf')):
        T.log(f"closing player {pid}: {cmd[:80]}"); os.kill(int(pid), 15)
pause(1.5)

# ---------------------------------------------------------------- 10 quit mid-render
step('Quit while rendering', 'the app asks first, stops the render cleanly, and offers to restart it next time')
click(*HDR['render'], button=3); pause(1.2)
if not press_any('Render “Draft', timeout=2):
    press_any('Queue “Draft')     # a render from chapter 9 is still running
pause(3)
press('Close', exact=True); pause(1.5); snap('c10-caution')
press('Quit', exact=True); wait_exit(40)
shutil.move(EXTRA, os.path.join(WORK, 'archive', 'zizzle-extra.mp4'))   # the file "goes missing"
step('Reopen', 'the last project opens by itself, and the unfinished render is offered')
launch(); pause(3); snap('c10-relaunch')
press('Restart', exact=True); pause(2.5); rest(); snap('c10-restarted')
step('Missing media', 'a moved file shows as red striped clips and a banner; rendering asks you to relink first')
if find('Relink First', exact=True, timeout=3):
    snap('c10-warning'); press('Relink First', exact=True)
else:
    press('Relink…')
pause(2); center_dialogs(0.6); snap('c10-relink-dialog')
step('Relink', 'Search a Folder finds the file by name and fingerprint; one undo step')
press('Search a Folder…'); center_dialogs(1.0)
set_location(os.path.join(WORK, 'archive') + '/'); pause(0.5)
if not (press('Select', exact=True) or press('Open', exact=True)):
    keysym(K_RETURN)
pause(3); T.log(f"relink status {status()!r}"); snap('c10-relinked')
if find('Close', exact=True, timeout=1):
    keysym(K_ESC)
pause(1)
if find('Cancel Render', exact=True, timeout=1):
    press('Cancel Render', exact=True)
elif 'Rendering' in status():
    click(*HDR['render']); pause(1.5); press('Cancel Render', exact=True)
pause(1.5)

# ---------------------------------------------------------------- 11 help
step('Help', 'collapsible sections that remember where you were: every control and every shortcut')
press('Help'); pause(2.5)
# The expander rows expose no AT-SPI action; click their headers (the dialog is
# centred). Timeline first: opening it doesn't move Edit Transform above it.
for y in (632, 522):
    click(910, y); pause(1.5)
snap('c11-help')
step('Release notes', 'what is new in each release, written for testers')
press_any('Release Notes'); pause(2); snap('c11-notes')
step('About and diagnostics', 'Copy Diagnostics puts versions and recent log lines on the clipboard for a bug report')
press_any('About'); pause(2); press_any('Copy Diagnostics'); pause(2); snap('c11-about')
keysym(K_ESC); pause(1)

step('U Stu Video Editor', 'built with GTK4, libadwaita and MLT')
act('seek-home'); act('play-pause'); pause(6); act('play-pause'); pause(2)
quit_app(40)
