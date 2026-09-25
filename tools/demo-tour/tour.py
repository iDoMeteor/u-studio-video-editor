# u Studio demo tour. Chapters run in order; TOUR_UPTO stops early for testing.
import os, sys, time
sys.path.insert(0, os.environ['TOUR_DEMO'])
import tourlib as T
from tourlib import *
M = os.environ['TOUR_MEDIA']
WORK = os.path.join(os.environ['TOUR_OUT'], 'work')
UPTO = int(os.environ.get('TOUR_UPTO', '99'))
SHOTS = os.environ.get('TOUR_SHOTS', '1') == '1'
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
def step(title, sub=''):
    global n
    n += 1
    if n > UPTO:
        quit_app(); sys.exit(0)
    chapter(title, sub)

launch()
step('u Studio', 'a GNOME-native multi-track video editor')
pause(3)

# ---------------------------------------------------------------- 1
step('Import', 'pick several files: they land in order on the active track, and the project takes its format from the first')
import_folder(M + '/unicorn')
pause(6); rest(); snap('c1-imported')
step('Clip tooltips', 'rest the pointer on a clip: the frame under it, its timecode and details')
hover(520, row_y(0), 3.0); snap('c1-tooltip')
rest()

# ---------------------------------------------------------------- 2
step('Mixed frame rates', 'a 25 fps 1344×768 set joins a 30 fps project; MLT maps every frame by time')
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
step('Playback', 'real-time playback on its own engine thread, with sound')
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

# ---------------------------------------------------------------- 6 save
PROJECT = os.path.join(WORK, 'unicorn-demo.ustudio')
HDR = {'save': (1550, 27), 'render': (1642, 27), 'settings': (1736, 27), 'help': (1772, 27)}
def name_entry_set(path):
    for n in walk(app_node()):
        nm, ds, rl = info(n)
        if rl in ('text', 'entry') and nm.startswith('Name'):
            n.get_editable_text_iface().set_text_contents(path); return True
    return False

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
                 ('Performance', 'preview scale, worker threads, thumbnail and waveform jobs'),
                 ('Locations', 'default project and export folders'),
                 ('Render', 'render profiles (High quality is the default) and render threads'),
                 ('Drop-ins', 'optional effects, titles and more, installed separately'),
                 ('Shortcuts', 'every action and its key')]:
    step(f'Settings: {tab}', sub)
    click(TABS[tab], 776); pause(3.2); snap(f'c7-{tab.lower()}')
keysym(K_ESC); pause(1)

# ---------------------------------------------------------------- 8 project frame rate
step('Project frame rate', 'click the format under the title; everything stays in time')
press('unicorn-demo'); pause(1.5); snap('c8-dialog')
click(960, 566); pause(1.2); snap('c8-list')
keysym(0xff52); pause(0.4); keysym(0xff52); pause(0.4); keysym(K_RETURN); pause(1)
press('Change', exact=True); pause(2.5); T.log(f"fps status {status()!r}"); snap('c8-changed')
act('undo'); pause(1.5)

# ---------------------------------------------------------------- 9 render
def wait_status(prefixes, timeout):
    end = time.time() + timeout
    while time.time() < end:
        st = status()
        if st.startswith(prefixes):
            return st
        time.sleep(0.5)
    return status()

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
ff_start(); st = wait_status(('Rendered',), 240); ff_end(); T.log(f"render status {st!r}"); rest(); pause(2); snap('c9-done')
click(*HDR['render']); pause(4); snap('c9-player')
# close whatever player GIO launched on :97
import subprocess as _sp
for pid in os.listdir('/proc'):
    if not pid.isdigit() or int(pid) in (os.getpid(), T.proc.pid): continue
    try:
        env = open(f'/proc/{pid}/environ', 'rb').read().split(b'\0')
        cmd = open(f'/proc/{pid}/cmdline', 'rb').read().replace(b'\0', b' ').decode(errors='replace')
    except Exception:
        continue
    if b'DISPLAY=:97' in env and b'TOUR_OUT=' not in env and not any(k in cmd for k in ('Xvfb', 'at-spi', 'dbus', 'ffmpeg', 'audiorec', 'python3', 'u-studio-video-editor', 'xdg-desktop-portal', 'goa', 'gvfs', 'dconf')):
        T.log(f"closing player {pid}: {cmd[:80]}"); os.kill(int(pid), 15)
pause(1.5)

# ---------------------------------------------------------------- 10 quit mid-render
step('Quit while rendering', 'the app asks first, stops the render cleanly, and offers to restart it next time')
click(*HDR['render'], button=3); pause(1.2); press_any('Render “Draft'); pause(3)
press('Close', exact=True); pause(1.5); snap('c10-caution')
press('Quit', exact=True); wait_exit(40)
step('Reopen', 'the last project opens by itself, and the unfinished render is offered')
launch(); pause(3); snap('c10-relaunch')
press('Restart', exact=True); pause(4); rest(); snap('c10-restarted')
click(*HDR['render']); pause(1.5); press('Cancel Render', exact=True); pause(1.5)

# ---------------------------------------------------------------- 11 help
step('Help', 'every control explained, and every shortcut')
press('Help'); pause(3); snap('c11-help')
keysym(K_ESC); pause(1)

step('u Studio', 'built with GTK4, libadwaita and MLT')
act('seek-home'); act('play-pause'); pause(6); act('play-pause'); pause(2)
quit_app(40)
