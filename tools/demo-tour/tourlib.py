# Demo-tour driving library: runs inside dbus-run-session on a private Xvfb (run_tour.sh picks the display).
import gi, os, subprocess, sys, time, json
gi.require_version('Atspi', '2.0')
from gi.repository import Atspi
from PIL import Image

DEMO = os.path.dirname(os.path.abspath(__file__))
OUT = os.environ['TOUR_OUT']
BIN = os.environ['TOUR_BIN']
T0 = float(os.environ.get('TOUR_T0', time.time()))
LOG = open(os.path.join(OUT, 'tour.log'), 'a')
CHAPTERS = []
FF = []
proc = None

def now():
    return time.time() - T0

def log(m):
    print(f"{now():8.2f}  {m}", file=LOG, flush=True)

def chapter(title, sub=''):
    CHAPTERS.append((now(), title, sub))
    json.dump(CHAPTERS, open(os.path.join(OUT, 'chapters.json'), 'w'))
    log(f"=== {title} {sub}")

def pause(s):
    time.sleep(s)

# ---------- app lifecycle
def launch(extra_env=None):
    global proc
    env = dict(os.environ)
    env.update(extra_env or {})
    proc = subprocess.Popen([BIN], env=env, stdout=open(os.path.join(OUT, 'app.stdout'), 'a'),
                            stderr=open(os.path.join(OUT, 'app.stderr'), 'a'))
    log(f"launched pid {proc.pid}")
    for _ in range(100):
        if app_node():
            break
        time.sleep(0.2)
    subprocess.run(['python3', os.path.join(DEMO, 'fitwin.py')], capture_output=True, timeout=30)
    time.sleep(1.5)
    return proc

def alive():
    return proc is not None and proc.poll() is None

def quit_app(timeout=30):
    if alive():
        proc.terminate()
    wait_exit(timeout)

def find_any(text, timeout=6, exact=False):
    return find(text, roles=('button', 'toggle button', 'push button', 'menu item', 'check menu item',
                              'radio menu item', 'page tab', 'list item', 'label', 'combo box', 'radio button'),
                exact=exact, timeout=timeout)

def press_any(text, exact=False, timeout=6):
    n = find_any(text, timeout=timeout, exact=exact)
    if not n:
        log(f"press_any '{text}': NOT FOUND"); return False
    try:
        ai = n.get_action_iface()
        for i in range(ai.get_n_actions()):
            if ai.get_action_name(i) in ('click', 'activate', 'press', 'toggle', 'select'):
                ai.do_action(i); log(f"press_any '{text}'"); return True
    except Exception:
        pass
    # fall back to a real click at its extents (GTK4 reports window-relative)
    log(f"press_any '{text}': no action"); return False

def wait_exit(timeout=60):
    end = time.time() + timeout
    while alive() and time.time() < end:
        time.sleep(0.3)
    log(f"app exit: {proc.poll()}")

# ---------- AT-SPI
TARGET = None   # None: the editor we launched; else another app's AT-SPI name ('u-studio-titles')

def use_app(name=None):
    """Point find/press/walk at another app (U Stu Titles, which the editor starts), or back."""
    global TARGET
    TARGET = name

def app_node():
    d = Atspi.get_desktop(0)
    for i in range(d.get_child_count()):
        a = d.get_child_at_index(i)
        try:
            if a and (a.get_name() == TARGET if TARGET else proc and a.get_process_id() == proc.pid):
                return a
        except Exception:
            pass
    return None

def extents(node):
    """Screen box (x, y, w, h). GTK4's SCREEN extents are unreliable, but summing
    PARENT extents up to the frame works (tools/titles-smoke/drive.py); the
    windows sit at the screen's origin here (fitwin.py, no WM)."""
    x = y = 0
    n = node
    while n is not None and n.get_role_name() != 'frame':
        e = n.get_extents(Atspi.CoordType.PARENT)
        x += e.x; y += e.y
        n = n.get_parent()
    e = node.get_extents(Atspi.CoordType.PARENT)
    return x, y, e.width, e.height

FRAME_OFF = (12, 11)   # frame extents include the CSD shadow: screen = extents + this (measured)

def centre_of(node):
    x, y, w, h = extents(node)
    return x + w // 2 + FRAME_OFF[0], y + h // 2 + FRAME_OFF[1]

def where(text, roles=('button', 'toggle button', 'push button'), exact=False, timeout=6):
    """Screen centre of a widget found by name, or None."""
    n = find(text, roles=roles, exact=exact, timeout=timeout)
    return centre_of(n) if n else None

def dump_tree(path, maxlen=60):
    """Every named node of the current app, with role and box: for writing chapters."""
    with open(path, 'w') as f:
        for n in walk(app_node()):
            nm, ds, rl = info(n)
            if nm or ds:
                try: box = extents(n)
                except Exception: box = None
                f.write(f"{rl:18} {nm[:maxlen]!r:64} {ds[:40]!r:44} {box}\n")

def walk(n, depth=0):
    if n is None or depth > 60:
        return
    yield n
    try:
        c = n.get_child_count()
    except Exception:
        return
    for i in range(c):
        try:
            yield from walk(n.get_child_at_index(i), depth + 1)
        except Exception:
            pass

def info(n):
    try: nm = n.get_name() or ''
    except Exception: nm = ''
    try: ds = n.get_description() or ''
    except Exception: ds = ''
    try: rl = n.get_role_name()
    except Exception: rl = '?'
    return nm, ds, rl

def find(text, roles=('button', 'toggle button', 'push button'), exact=False, timeout=8):
    end = time.time() + timeout
    while time.time() < end:
        for n in walk(app_node()):
            nm, ds, rl = info(n)
            if rl not in roles:
                continue
            if (nm == text or ds == text) if exact else (nm.startswith(text) or ds.startswith(text)):
                return n
        time.sleep(0.3)
    return None

def press(text, **kw):
    n = find(text, **kw)
    if not n:
        log(f"press '{text}': NOT FOUND"); return False
    ai = n.get_action_iface()
    for i in range(ai.get_n_actions()):
        if ai.get_action_name(i) in ('click', 'activate', 'press', 'toggle'):
            ai.do_action(i); log(f"press '{text}'"); return True
    log(f"press '{text}': no action"); return False

def press_in(window, text, timeout=6):
    """press() limited to the top-level window titled `window`, so a common
    label like 'Close' can't hit the main window's button."""
    end = time.time() + timeout
    while time.time() < end:
        app = app_node()
        for i in range(app.get_child_count() if app else 0):
            w = app.get_child_at_index(i)
            if w is None or info(w)[0] != window:
                continue
            for n in walk(w):
                nm, ds, rl = info(n)
                if rl in ('button', 'push button') and (nm == text or ds == text):
                    ai = n.get_action_iface()
                    for j in range(ai.get_n_actions()):
                        if ai.get_action_name(j) in ('click', 'activate', 'press'):
                            ai.do_action(j); log(f"press '{text}' in '{window}'"); return True
        time.sleep(0.3)
    log(f"press '{text}' in '{window}': NOT FOUND"); return False

def act(name, n=1, gap=0.35, param=None):
    for _ in range(n):
        args = ['gdbus', 'call', '--session', '--dest', 'com.ustudio.VideoEditor', '--object-path',
                '/com/ustudio/VideoEditor/window/1', '--method', 'org.gtk.Actions.Activate', name,
                param if param else '[]', '{}']
        r = subprocess.run(args, capture_output=True, text=True, timeout=30)
        if r.returncode != 0:
            log(f"act {name}: FAILED {r.stderr.strip()[:160]}"); return False
        time.sleep(gap)
    log(f"act {name} x{n}")
    return True

# ---------- input (XTest via AT-SPI)
KC = {'ctrl': 37, 'shift': 50, 'alt': 64}
def mouse(x, y, ev):
    Atspi.generate_mouse_event(int(x), int(y), ev)

def move(x, y, steps=12, dur=0.35):
    # glide from the last known position for a watchable cursor
    global _mx, _my
    sx, sy = _mx, _my
    for i in range(1, steps + 1):
        mouse(sx + (x - sx) * i / steps, sy + (y - sy) * i / steps, 'abs')
        time.sleep(dur / steps)
    _mx, _my = x, y
_mx, _my = 960, 540

def click(x, y, button=1, glide=True):
    if glide: move(x, y)
    mouse(x, y, f'b{button}c'); time.sleep(0.25)

def dclick(x, y):
    move(x, y); mouse(x, y, 'b1d'); time.sleep(0.3)

def drag(x0, y0, x1, y1, steps=25, dur=0.9, mods=()):
    move(x0, y0)
    for m in mods: key(m, True)
    mouse(x0, y0, 'b1p'); time.sleep(0.15)
    global _mx, _my
    for i in range(1, steps + 1):
        mouse(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, 'abs'); time.sleep(dur / steps)
    mouse(x1, y1, 'b1r'); time.sleep(0.2)
    for m in mods: key(m, False)
    _mx, _my = x1, y1
    time.sleep(0.4)
    log(f"drag {mods} ({x0},{y0})->({x1},{y1})")

def key(name, down):
    Atspi.generate_keyboard_event(KC[name], None, Atspi.KeySynthType.PRESS if down else Atspi.KeySynthType.RELEASE)
    time.sleep(0.08)

def keysym(sym, mods=()):
    for m in mods: key(m, True)
    Atspi.generate_keyboard_event(sym, None, Atspi.KeySynthType.SYM)
    time.sleep(0.08)
    for m in reversed(mods): key(m, False)
    time.sleep(0.2)

def typestr(s):
    Atspi.generate_keyboard_event(0, s, Atspi.KeySynthType.STRING)
    time.sleep(0.3)

# X keysyms
K_RETURN, K_ESC, K_TAB, K_DEL = 0xff0d, 0xff1b, 0xff09, 0xffff
K_LEFT, K_RIGHT, K_HOME, K_END = 0xff51, 0xff53, 0xff50, 0xff57

# ---------- screen analysis
def shot(name=None):
    p = os.path.join(OUT, (name or 'probe') + '.png')
    subprocess.run(['import', '-window', 'root', p], capture_output=True, timeout=30)
    if name: log(f"shot {name}")
    return p

def clips_in_row(y, x0=40, x1=1905, img=None):
    """Horizontal runs at row y whose colour differs from the track background."""
    im = Image.open(img or shot()).convert('RGB')
    bg = im.getpixel((x1 - 3, y))
    runs, start = [], None
    for x in range(x0, x1):
        p = im.getpixel((x, y))
        diff = sum(abs(a - b) for a, b in zip(p, bg))
        if diff > 40 and start is None: start = x
        elif diff <= 40 and start is not None:
            if x - start > 6: runs.append((start, x - 1))
            start = None
    if start is not None: runs.append((start, x1))
    return runs

# ---------- window placement (no WM on Xvfb)
from Xlib import display as _xd, X as _X
_d = _xd.Display()
def _toplevels():
    root = _d.screen().root
    out = []
    for w in root.query_tree().children:
        try:
            a = w.get_attributes()
            if a.map_state != _X.IsViewable: continue
            g = w.get_geometry()
            out.append((w, g))
        except Exception:
            pass
    return out

def center_dialogs(settle=0.6):
    """Centre every mapped top-level smaller than the screen (dialogs, choosers)."""
    time.sleep(settle)
    SW, SH = _d.screen().width_in_pixels, _d.screen().height_in_pixels
    for w, g in _toplevels():
        if g.width >= SW - 10 and g.height >= SH - 10: continue
        if g.width < 120 or g.height < 80: continue      # tooltips, popovers
        w.configure(x=(SW - g.width) // 2, y=(SH - g.height) // 2)
    _d.sync()
    time.sleep(0.3)

def dialog_origin(timeout=8):
    """Top-left of the largest non-fullscreen top-level (the open dialog)."""
    end = time.time() + timeout
    while time.time() < end:
        r = _dialog_origin()
        if r: return r
        time.sleep(0.3)
    return None

def _dialog_origin():
    SW, SH = _d.screen().width_in_pixels, _d.screen().height_in_pixels
    best = None
    for w, g in _toplevels():
        if g.width >= SW - 10 and g.height >= SH - 10: continue
        if best is None or g.width * g.height > best[1] * best[2]:
            tr = w.translate_coords(_d.screen().root, 0, 0)
            best = ((-tr.x, -tr.y), g.width, g.height)
    return best

CYAN = (25, 227, 255)
def cyan_runs(y, img=None, x0=30, x1=1910, tol=90):
    im = Image.open(img or shot()).convert('RGB')
    runs, start = [], None
    for x in range(x0, x1):
        p = im.getpixel((x, y))
        hit = sum(abs(a - b) for a, b in zip(p, CYAN)) < tol
        if hit and start is None: start = x
        elif not hit and start is not None:
            runs.append((start, x - 1)); start = None
    if start is not None: runs.append((start, x1))
    return runs

def selected_box(row_top, x_hint=None, timeout=2.5):
    """Box (x0, x1) of the cyan selection outline on a track row (retries while it draws)."""
    end = time.time() + timeout
    while True:
        r = _selected_box(row_top, x_hint)
        if r or time.time() > end:
            return r
        time.sleep(0.3)

def _selected_box(row_top, x_hint=None):
    img = shot()
    for dy in range(14, 24):
        runs = [r for r in cyan_runs(row_top + dy, img) if r[1] - r[0] > 8]
        if runs:
            if x_hint is not None:
                runs.sort(key=lambda r: 0 if r[0] <= x_hint <= r[1] else min(abs(r[0]-x_hint), abs(r[1]-x_hint)))
            return runs[0]
    return None

def focused_text(timeout=4):
    end = time.time() + timeout
    while time.time() < end:
        for n in walk(app_node()):
            try:
                if n.get_role_name() in ('text', 'entry') and n.get_state_set().contains(Atspi.StateType.FOCUSED):
                    return n
            except Exception:
                pass
        time.sleep(0.2)
    return None

def set_location(path):
    """Ctrl+L in a GTK file chooser, then set the entry text directly (no autocomplete race)."""
    keysym(ord('l'), mods=('ctrl',)); time.sleep(0.5)
    n = focused_text()
    if n is None:
        log("set_location: no focused entry, typing"); typestr(path); return False
    et = n.get_editable_text_iface()
    et.set_text_contents(path); time.sleep(0.6)
    log(f"set_location {path}")
    return True

def status():
    """Latest status-bar text, from the app's debug log."""
    try:
        lines = open(os.path.join(OUT, 'app.stderr'), errors='replace').read().splitlines()
    except FileNotFoundError:
        return ''
    for l in reversed(lines):
        i = l.find('[app] status: ')
        if i >= 0:
            return l[i + len('[app] status: '):]
    return ''

def edited(since):
    """True unless the status line changed to a refusal since `since`."""
    s = status()
    return not (s != since and s.startswith(("Can't", "Couldn't")))

def rest():
    move(960, 1005, dur=0.3)     # park the pointer on the scrollbar row: no tooltips

def ff_start():
    FF.append([now(), None])
def ff_end():
    FF[-1][1] = now()
    json.dump(FF, open(os.path.join(OUT, 'ff.json'), 'w'))

def _preview_cyan(y0=52, y1=718, x0=5, x1=1915):
    im = Image.open(shot()).convert('RGB'); pts = []
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            p = im.getpixel((x, y))
            if p[2] > 200 and p[1] > 180 and p[0] < 90:
                pts.append((x, y))
    return pts

def preview_box():
    """Bounding box of the cyan transform outline on the preview, without the
    rotation knob above it: the top and bottom come from the left edge only."""
    pts = _preview_cyan()
    if not pts:
        return None
    xa = min(x for x, _ in pts); xb = max(x for x, _ in pts)
    edge = [y for x, y in pts if x <= xa + 6]
    return (xa, min(edge), xb, max(edge))

def preview_knob():
    """Centre of the rotation knob: the topmost cyan above the box's middle."""
    b = preview_box()
    if not b:
        return None
    xm = (b[0] + b[2]) // 2
    ys = [y for x, y in _preview_cyan() if abs(x - xm) <= 8 and y < b[1] - 4]
    return (xm, min(ys) + 6) if ys else None
