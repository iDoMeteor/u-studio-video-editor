# Drives one app session for sanitizer runs. Runs INSIDE dbus-run-session so the app
# can't forward to the owner's running editor and gdbus can only reach our instance.
import gi, os, subprocess, sys, time
gi.require_version('Atspi', '2.0')
from gi.repository import Atspi

binary, outdir, mode = sys.argv[1], sys.argv[2], sys.argv[3]   # mode: explore | full
T0 = time.monotonic()
LOG = open(os.path.join(outdir, 'driver.log'), 'a')
def log(m): print(f"{time.monotonic()-T0:8.2f}  {m}", file=LOG, flush=True)

env = dict(os.environ)
proc = subprocess.Popen([binary], env=env, stdout=open(os.path.join(outdir, 'app.stdout'), 'w'),
                        stderr=open(os.path.join(outdir, 'app.stderr'), 'w'))
log(f"launched pid {proc.pid}")
import atexit, signal
def _cleanup():
    if proc.poll() is None:
        log("driver exiting with app still running: SIGTERM"); proc.terminate()
        try: proc.wait(60)
        except Exception: proc.kill(); proc.wait(10)
atexit.register(_cleanup)
signal.signal(signal.SIGTERM, lambda *a: sys.exit(1))

def app_node():
    d = Atspi.get_desktop(0)
    for i in range(d.get_child_count()):
        a = d.get_child_at_index(i)
        try:
            if a and a.get_process_id() == proc.pid: return a
        except Exception: pass
    return None

def walk(n, depth=0):
    if n is None or depth > 40: return
    yield n, depth
    try: c = n.get_child_count()
    except Exception: return
    for i in range(c):
        try: yield from walk(n.get_child_at_index(i), depth + 1)
        except Exception: pass

def label(n):
    try: nm = n.get_name() or ''
    except Exception: nm = ''
    try: ds = n.get_description() or ''
    except Exception: ds = ''
    try: rl = n.get_role_name()
    except Exception: rl = '?'
    return nm, ds, rl

def find(text, role='button', timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        a = app_node()
        for n, _ in walk(a):
            nm, ds, rl = label(n)
            if rl == role and (nm == text or ds == text or nm.startswith(text)): return n
        time.sleep(0.5)
    return None

def click(text, timeout=20):
    n = find(text, timeout=timeout)
    if not n: log(f"click '{text}': NOT FOUND"); return False
    ai = n.get_action_iface()
    for i in range(ai.get_n_actions()):
        if ai.get_action_name(i) in ('click', 'activate', 'press'):
            ai.do_action(i); log(f"click '{text}'"); return True
    log(f"click '{text}': no action"); return False

def act(name, n=1, pause=0.4):
    for _ in range(n):
        r = subprocess.run(['gdbus', 'call', '--session', '--dest', 'com.ustudio.VideoEditor', '--object-path',
                            '/com/ustudio/VideoEditor/window/1', '--method', 'org.gtk.Actions.Activate', name, '[]', '{}'],
                           capture_output=True, text=True, timeout=30)
        if r.returncode != 0: log(f"act {name}: FAILED {r.stderr.strip()[:200]}"); return
        time.sleep(pause)
    log(f"act {name} x{n}")

def wait(s, why=''): log(f"wait {s}s {why}"); time.sleep(s)

# --- wait for the window
for _ in range(240):
    if app_node() and find('Recover', timeout=0.1) or proc.poll() is not None: break
    time.sleep(0.5)
if proc.poll() is not None: log(f"app exited early rc={proc.returncode}"); sys.exit(1)

if mode == 'explore':
    with open(os.path.join(outdir, 'tree.txt'), 'w') as f:
        for n, d in walk(app_node()):
            nm, ds, rl = label(n)
            if rl in ('button', 'toggle button', 'dialog', 'alert', 'frame', 'push button'):
                f.write(f"{'  '*d}{rl}: name='{nm}' desc='{ds}'\n")
    r = subprocess.run(['gdbus', 'introspect', '--session', '--dest', 'com.ustudio.VideoEditor', '--object-path',
                        '/com/ustudio/VideoEditor/window/1'], capture_output=True, text=True)
    open(os.path.join(outdir, 'introspect.txt'), 'w').write(r.stdout + r.stderr)
    proc.terminate(); proc.wait(30); sys.exit(0)

def extents(n):
    c = n.get_component_iface()
    e = c.get_extents(Atspi.CoordType.SCREEN)
    return e.x, e.y, e.width, e.height

def mouse(x, y, ev):
    Atspi.generate_mouse_event(int(x), int(y), ev)

def drag(x0, y0, x1, y1, steps=10, button=1, pause=0.03):
    mouse(x0, y0, 'abs'); time.sleep(0.1)
    mouse(x0, y0, f'b{button}p'); time.sleep(0.1)
    for i in range(1, steps + 1):
        mouse(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, 'abs'); time.sleep(pause)
    mouse(x1, y1, f'b{button}r'); time.sleep(0.3)
    log(f"drag ({x0},{y0})->({x1},{y1})")

def alive():
    return proc.poll() is None

def shot(name):
    p = os.path.join(outdir, name + '.png')
    subprocess.run(['import', '-window', 'root', p], capture_output=True, timeout=30)
    log(f"shot {name}")
    return p

KEYS = {'ctrl': 37, 'shift': 50, 'alt': 64}
def key(name, down):
    Atspi.generate_keyboard_event(KEYS[name], None, Atspi.KeySynthType.PRESS if down else Atspi.KeySynthType.RELEASE)
    time.sleep(0.1)

def mdrag(mod, *a, **k):
    key(mod, True); drag(*a, **k); key(mod, False)

def status():
    for n, _ in walk(app_node()):
        nm, ds, rl = label(n)
        if rl == 'label' and nm and any(w in nm for w in ("Can't", "Couldn't", 'Copied', 'Slipped', 'Created', 'Moved', 'Resized', 'Removed', 'Deleted', 'Undid', 'Redid', 'Marker', 'Closed')):
            return nm
    return ''

def step(desc, fn):
    if not alive(): log(f"SKIP {desc}: app gone rc={proc.poll()}"); return
    fn(); time.sleep(0.8)
    log(f"  -> {desc}: status='{status()}' alive={alive()}")
