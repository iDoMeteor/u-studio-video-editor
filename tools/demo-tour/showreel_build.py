# Builds the showreel as a U Stu project and renders it with U Stu: the
# footage on V1 (with spare frames at each cut for the transitions), the
# music on A1 (split from a black music video), a section title from U Stu
# Titles on V2 at each section, a transition style at every cut, looks on the
# bookend clips, then Save and Render (High quality). Runs as TOUR_SCRIPT;
# the plan is <TOUR_OUT>/plan.json (made by showreel_plan.py).
import os, sys, time, json, subprocess
sys.path.insert(0, os.environ['TOUR_DEMO'])
import tourlib as T
from tourlib import *
OUT = os.environ['TOUR_OUT']
PLAN = json.load(open(os.path.join(OUT, 'plan.json')))
FPS = 30
TG = ('toggle button',)

def snap(n): shot(n)
def is_on(node):
    st = node.get_state_set()
    return st.contains(Atspi.StateType.PRESSED) or st.contains(Atspi.StateType.CHECKED)
def inspector(open_=False):
    n = find('Inspector', roles=TG, timeout=4)
    if n and is_on(n) != open_:
        click(*centre_of(n)); pause(0.8)
def seek(f):
    sl = find('Seek', roles=('slider',), timeout=3)
    if sl: sl.get_value_iface().set_current_value(float(f)); pause(0.5)
def import_path(path, folder=False):
    act('import'); center_dialogs(1.0)
    if folder:
        set_location(path.rstrip('/') + '/'); keysym(K_RETURN); pause(1.5)
        (dx, dy), dw, dh = dialog_origin()
        click(dx + 300, dy + 93); keysym(ord('a'), mods=('ctrl',)); pause(0.8)
        press('Open', exact=True)
    else:
        set_location(path); keysym(K_RETURN); pause(1.2)
        if find('Open', exact=True, timeout=1):
            press('Open', exact=True)
def wait_status(prefixes, timeout):
    end = time.time() + timeout
    while time.time() < end:
        st = status()
        if st.startswith(prefixes):
            return st
        time.sleep(1)
    return status()
def titles_window():
    use_app('u-studio-titles')
    for _ in range(40):
        if app_node(): break
        time.sleep(0.5)
    subprocess.run(['python3', os.path.join(T.DEMO, 'fitwin.py'), 'u-studio-titles'], capture_output=True, timeout=30)
    pause(1.5)
def clip_bands(y0=420, y1=980):
    """Centres of the timeline's clip bands, top to bottom: rows whose clips cover
    most of a line, at least 16 px tall (the ruler's text isn't)."""
    img = shot()
    rows = [y for y in range(y0, y1, 2) if sum(b - a for a, b in clips_in_row(y, x0=40, x1=1880, img=img)) > 600]
    bands, cur = [], []
    for y in rows:
        if cur and y - cur[-1] > 4:
            bands.append(cur); cur = []
        cur.append(y)
    if cur: bands.append(cur)
    return [(b[0] + b[-1]) // 2 for b in bands if b[-1] - b[0] >= 16]

segs = PLAN['segments']                      # [{file, content, pad}] seconds
starts = []                                  # final sequence frames, after the pads are removed
t = 0
for s in segs:
    starts.append(t); t += int(round(s['content'] * FPS))
T.log(f"plan: {len(segs)} segments, {t / FPS:.1f} s")

launch(); inspector(False)

# 1. The music: a black 1080p30 video with the songs, split, picture deleted.
import_path(PLAN['music_video']); pause(5); rest()
MB = clip_bands(); T.log(f'music bands {MB}')
click(300, MB[0], button=3); pause(1.2); press('Split Audio'); pause(3); rest()
click(300, MB[0]); pause(0.5); act('delete-selected-clip'); pause(1.5)
T.log(f"music status {status()!r}"); snap('b1-music')

# 2. The footage, in name order on V1.
import_path(PLAN['segments_dir'], folder=True); pause(10); rest()
act('zoom-fit'); pause(1); snap('b2-footage')
BANDS = clip_bands(); T.log(f'bands {BANDS}')
if len(BANDS) < 2:
    snap('ERR-bands'); raise SystemExit(f'expected V1 and A1 bands, got {BANDS}')
V1, A1 = BANDS[0], BANDS[1]
PITCH = A1 - V1

# 3. Spare frames at each cut: split each padded segment at its content's end and
#    ripple-delete the tail, last to first so earlier positions don't move.
padded = []
t = 0
for s in segs:
    padded.append(t); t += int(round((s['content'] + s['pad']) * FPS))
for i in range(len(segs) - 1, -1, -1):
    if segs[i]['pad'] <= 0:
        continue
    end = padded[i] + int(round(segs[i]['content'] * FPS))
    seek(end); act('split-at-playhead'); pause(0.8)
    seek(end - 1); act('select-next-clip'); pause(0.4); act('ripple-delete-selected'); pause(1.2)
    T.log(f"trim {i}: {status()!r}")
rest(); act('zoom-fit'); pause(1); snap('b3-trimmed')

# 4. Looks on the bookend clips (select by clicking the clip on V1).
for i, look in PLAN.get('looks', {}).items():
    i = int(i)
    click(1885, V1); pause(0.4)                    # V1 active, nothing selected
    if i == 0:
        seek(10); act('select-previous-clip')          # nothing selected: the clip under the playhead
    else:
        seek(starts[i] - 1); act('select-next-clip')
    pause(0.6)
    act('effects-browser'); pause(1.5)
    sfield = find('Search effects', roles=('entry', 'text'), timeout=3)
    if sfield:
        click(*centre_of(sfield)); pause(0.3); keysym(ord('a'), mods=('ctrl',)); typestr(look.lower()); pause(2.5)
    tile = find(look, roles=('table cell',), exact=True, timeout=3)
    if tile: click(*centre_of(tile)); pause(2)
    clip = [info(n)[0] for n in walk(app_node()) if info(n)[0].startswith('Clip: ')]
    T.log(f"look {look} on {i}: {status()!r} {clip[:1]}")
    inspector(False)

# 5. Transitions at every cut on V1 (T acts on the active track: V1).
click(1885, V1); pause(0.5)
styles = PLAN['transitions']
for k, i in enumerate(range(1, len(segs))):
    style = styles[k % len(styles)]
    seek(starts[i] - 4); act('effects-add-transition'); pause(2.5)
    tile = find(style, roles=('table cell',), exact=False, timeout=4)
    if tile: click(*centre_of(tile)); pause(1.2)
    T.log(f"cut {i} @{starts[i]} {style}: tile {bool(tile)} {status()!r}")
inspector(False); act('zoom-fit'); pause(1); snap('b5-transitions')

# 6. A track for the titles, on top; a title from U Stu Titles at each section.
press('Add track'); pause(1.2); act('zoom-fit'); pause(0.8)
NB = clip_bands(); T.log(f'bands after Add track {NB}')
V2 = NB[0] - PITCH                                  # the new, empty top track
click(1885, V2); pause(0.5)                        # make it the active track
for sec in PLAN['sections'][:PLAN.get('titles_max', 99)]:
    at = starts[sec['segment']] + int(round(sec.get('offset', 1.2) * FPS))   # after the cut's transition (a backdrop inside a Push Left hung, 2026-10-01)
    seek(at); act('titles-new'); pause(5)
    titles_window()
    if not press(sec['template'], exact=True):
        T.log(f"template {sec['template']!r} NOT FOUND")
    pause(2.5)
    # Play over the footage: the title's own background off (Kind: None).
    kind = find('Kind', roles=('combo box',), exact=True, timeout=3)
    if kind:
        click(*centre_of(kind)); pause(1.0)
        none = find_any('None', exact=True, timeout=2)
        if none: click(*centre_of(none)); pause(1.0)
        else: keysym(K_ESC)
    T.log(f"background kind combo {bool(kind)}"); snap(f"b6-designer-{sec['segment']}")
    keysym(ord('s'), mods=('ctrl',)); pause(2)
    press('Close', exact=True); pause(2)
    use_app(None)
    seek(at - 1); act('select-next-clip'); pause(0.6)
    inspector(True); pause(1.2)
    tab = find('Title', roles=('page tab',), exact=True, timeout=3)
    if tab: click(*centre_of(tab)); pause(1.2)
    for field, text in sec['fields'].items():
        e = find(field, roles=('text',), exact=True, timeout=3)
        if e:
            click(*centre_of(e)); pause(0.3); keysym(ord('a'), mods=('ctrl',)); typestr(text); pause(0.8)
        else:
            T.log(f"field {field!r} NOT FOUND for {sec['template']}")
    pause(1); snap(f"b6-title-{sec['segment']}")
    inspector(False)
    T.log(f"title {sec['template']} @{at}: {status()!r}")
act('zoom-fit'); pause(1); snap('b6-titles')

if not PLAN.get('render', True):
    quit_app(); sys.exit(0)
# 7. Save, then render with the default profile (High quality).
PROJECT = os.path.join(OUT, 'work', 'showreel.ustudio')
os.makedirs(os.path.dirname(PROJECT), exist_ok=True)
press('Save project'); center_dialogs(1.2)
for n in walk(app_node()):
    nm, ds, rl = info(n)
    if rl in ('text', 'entry') and nm.startswith('Name'):
        n.get_editable_text_iface().set_text_contents(PROJECT); break
pause(1); press('Save', exact=True); pause(2); T.log(f"save {status()!r}")
press('Render…'); center_dialogs(1.2); snap('b7-render-dialog')
press('Save', exact=True); pause(3)
st = wait_status(('Rendered', "Couldn't", 'Render failed'), 3600)
T.log(f"RENDER {st!r}")
snap('b7-done')
quit_app()
