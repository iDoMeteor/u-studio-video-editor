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

# --- full session
click('Recover'); wait(6, 'project load + engine reset')
act('play-pause'); wait(5, 'playing 1x across the dissolve and the still')
act('play-pause'); wait(1)
act('step-forward', 3); act('step-backward', 2)
act('seek-next-cut', 3); act('seek-previous-cut', 2)
act('shuttle-forward', 3, pause=1.5); act('shuttle-reverse', 2, pause=1.5); act('shuttle-stop'); wait(1)
act('seek-home'); act('step-forward-10', 2); act('loop-set-in'); act('step-forward-10', 6); act('loop-set-out')
act('play-pause'); wait(6, 'loop playback, should wrap several times'); act('play-pause')
act('seek-end'); act('step-backward-minute'); act('step-forward-minute'); act('seek-home')
act('active-track-down', 2); act('active-track-up', 3)
# edits, paused then while playing (every edit rebuilds the tractor and restarts the consumer)
act('step-forward-10', 5); click('Split at playhead'); wait(2)
act('undo'); act('redo'); act('undo')
click('Add track'); wait(1); act('undo')
act('play-pause'); wait(2, 'playing')
for i in range(8): act('redo', pause=0.2); act('undo', pause=0.2)
click('Split at playhead'); wait(2); act('undo'); wait(1)
act('active-track-down'); act('delete-selected-clip'); act('undo')
act('play-pause'); wait(1)
click('Media browser'); wait(6, 'thumbnails'); click('Media browser')
act('play-pause'); wait(3); act('play-pause')
# New Project while a project is loaded -> EngineSync::reset (audit E1 profile lifetime path)
click('New project'); wait(2); click('Discard'); wait(4)
act('play-pause'); wait(2); act('play-pause')
act('undo'); act('redo')
# quit
click('Close'); wait(2); click('Discard', timeout=5)
for _ in range(240):
    if proc.poll() is not None: break
    time.sleep(0.5)
if proc.poll() is None: log("app did not exit within 120s; terminating"); proc.terminate(); proc.wait(60)
log(f"app exit code {proc.returncode}")
