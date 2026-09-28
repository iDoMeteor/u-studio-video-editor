# Drives u-studio-titles on the private display (run.sh): XTest for the
# mouse and keys, AT-SPI to find the canvas and to reach dialog fields and
# buttons (with no window manager, XTest keys can't reach a new dialog).
import os, subprocess, sys, time
import xml.etree.ElementTree as ET
import gi
gi.require_version("Atspi", "2.0")
from gi.repository import Atspi
from Xlib import X, XK, display
from Xlib.ext import xtest

OUT = os.environ["SMOKE_OUT"]
APP = "u-studio-titles"
D = display.Display()
FAILED = []

def log(message):
    print(message, flush=True)

def check(name, ok, detail=""):
    log(("PASS " if ok else "FAIL ") + name + (f" ({detail})" if detail and not ok else ""))
    if not ok:
        FAILED.append(name)

# --- X input ---------------------------------------------------------------
def move(x, y):
    xtest.fake_input(D, X.MotionNotify, x=int(x), y=int(y)); D.sync(); time.sleep(0.03)

def button(down):
    xtest.fake_input(D, X.ButtonPress if down else X.ButtonRelease, 1); D.sync(); time.sleep(0.05)

def click(x, y):
    move(x, y); button(True); button(False); time.sleep(0.3)

def double_click(x, y):
    move(x, y); button(True); button(False); button(True); button(False); time.sleep(0.6)

def drag(x0, y0, x1, y1):
    move(x0, y0); button(True)
    for i in range(1, 25):
        move(x0 + (x1 - x0) * i / 24, y0 + (y1 - y0) * i / 24)
    button(False); time.sleep(0.4)

KEYSYMS = {" ": "space", ".": "period", "-": "minus", "/": "slash", "_": "underscore"}

def key(name, down):
    code = D.keysym_to_keycode(XK.string_to_keysym(name))
    xtest.fake_input(D, X.KeyPress if down else X.KeyRelease, code); D.sync(); time.sleep(0.03)

def press(*names):
    for n in names: key(n, True)
    for n in reversed(names): key(n, False)
    time.sleep(0.3)

def type_text(text):
    for ch in text:
        shift = ch.isupper()
        if shift: key("Shift_L", True)
        n = KEYSYMS.get(ch, ch.lower() if shift else ch)
        key(n, True); key(n, False)
        if shift: key("Shift_L", False)
    time.sleep(0.3)

def shot(name):
    subprocess.run(["import", "-window", "root", os.path.join(OUT, name + ".png")], capture_output=True, timeout=30)

# --- AT-SPI ----------------------------------------------------------------
def app():
    desktop = Atspi.get_desktop(0)
    for i in range(desktop.get_child_count()):
        a = desktop.get_child_at_index(i)
        if a and a.get_name() == APP:
            return a
    return None

def walk(node, depth=0):
    if node is None or depth > 60:
        return
    yield node
    for i in range(node.get_child_count()):
        yield from walk(node.get_child_at_index(i), depth + 1)

def find(pred, timeout=8):
    end = time.time() + timeout
    while time.time() < end:
        a = app()
        if a:
            for n in walk(a):
                try:
                    if pred(n):
                        return n
                except Exception:
                    pass
        time.sleep(0.2)
    return None

def showing(n):
    return n.get_state_set().contains(Atspi.StateType.SHOWING)

def extents(node):
    # GTK4 reports extents relative to the parent: add them up to the window
    # (at 0,0 with no window manager).
    x = y = 0
    n = node
    while n is not None and n.get_role_name() != "frame":
        e = n.get_extents(Atspi.CoordType.PARENT)
        x += e.x; y += e.y
        n = n.get_parent()
    e = node.get_extents(Atspi.CoordType.PARENT)
    return x, y, e.width, e.height

def press_button(label):
    node = find(lambda n: n.get_role_name() in ("button", "push button") and n.get_name() == label and showing(n))
    if not node:
        return False
    action = node.get_action_iface()
    for i in range(action.get_n_actions()):
        if action.get_action_name(i) in ("click", "activate", "press"):
            action.do_action(i)
            time.sleep(0.8)
            return True
    return False

def set_entry(text):
    node = find(lambda n: n.get_role_name() in ("text", "entry") and n.get_editable_text_iface() is not None
                and showing(n))
    if not node:
        return False
    node.get_editable_text_iface().set_text_contents(text)
    return True

# --- the canvas: title pixels to screen pixels (canvas.cpp's mapping) -------
class Canvas:
    def __init__(self, width=1920, height=1080, margin=24):
        node = find(lambda n: n.get_name() == "Title canvas")
        if not node:
            raise SystemExit("no canvas")
        x, y, w, h = extents(node)
        self.scale = min((w - 2 * margin) / width, (h - 2 * margin) / height)
        self.x = x + (w - width * self.scale) / 2
        self.y = y + (h - height * self.scale) / 2

    def at(self, cx, cy):
        return self.x + cx * self.scale, self.y + cy * self.scale

# --- the scenario ------------------------------------------------------------
def lower_third():
    if not find(lambda n: n.get_name() == "Title canvas", timeout=20):
        check("the designer starts", False)
        return
    subprocess.run([sys.executable, os.path.join(os.environ["SMOKE_HERE"], "..", "packaging-smoke", "fitwin.py")],
                   capture_output=True, timeout=30)
    time.sleep(1.5)
    canvas = Canvas()
    click(*canvas.at(960, 100)) # focus the canvas

    # A rounded bar (Ctrl+Shift+R: centred, 480 x 180), moved to the lower
    # left, then widened by its right handle.
    press("Control_L", "Shift_L", "r")
    drag(*canvas.at(960, 540), *canvas.at(440, 890))
    drag(*canvas.at(680, 890), *canvas.at(820, 890))
    shot("1-bar")

    # The name: new text, typed on the canvas, moved onto the bar.
    press("Control_L", "t")
    double_click(*canvas.at(960, 530))
    press("Control_L", "a")
    type_text("Ada Lovelace")
    press("Return")
    drag(*canvas.at(960, 535), *canvas.at(470, 850))
    shot("2-name")

    # The role, the same way, under the name.
    press("Control_L", "t")
    double_click(*canvas.at(960, 530))
    press("Control_L", "a")
    type_text("Host")
    press("Return")
    drag(*canvas.at(960, 535), *canvas.at(330, 945))
    shot("3-role")

    # Nothing selected: the title's own inspector, and Apply Brand.
    click(*canvas.at(1700, 200))
    check("Apply Brand is there with nothing selected", press_button("Apply Brand"))
    shot("4-brand")

    # Save through the Save dialog.
    press("Control_L", "s")
    time.sleep(2)
    check("the Save dialog takes a name", set_entry("lower-third.ustitle"))
    check("Save", press_button("Save"))
    time.sleep(1.5)
    shot("5-saved")

    path = os.path.join(OUT, "home", "lower-third.ustitle")
    check("the title is saved", os.path.exists(path), path)
    if not os.path.exists(path):
        return
    root = ET.parse(path).getroot()
    layers = root.findall("layer")
    check("three layers", len(layers) == 3, str(len(layers)))
    kinds = [l.get("kind") for l in layers]
    check("a shape and two texts", kinds == ["shape", "text", "text"], str(kinds))
    texts = [l.findtext("text") for l in layers if l.get("kind") == "text"]
    check("the typed texts", texts == ["Ada Lovelace", "Host"], str(texts))
    bar = layers[0]
    check("a rounded bar", bar.get("shape") == "rounded-rect", bar.get("shape"))
    x, y, w = float(bar.get("x")), float(bar.get("y")), float(bar.get("w"))
    check("the bar is at the lower left, wider", x < 400 and y > 700 and w > 550, f"x={x} y={y} w={w}")
    fonts = [l.find("font").get("family") for l in layers if l.get("kind") == "text"]
    check("brand fonts", all(f in ("Anton", "Space Grotesk") for f in fonts), str(fonts))
    check("the name in the brand gradient", layers[1].find("fill").get("gradient") == "linear")

    # Export it with alpha (the default, ProRes 4444) through the dialogs.
    press("Control_L", "e")
    check("the Export dialog opens", press_button("Export…"))
    time.sleep(1.5)
    check("the export takes a name", set_entry("lower-third.mov"))
    check("Save the export", press_button("Save"))
    movie = os.path.join(OUT, "home", "lower-third.mov")
    end = time.time() + 60
    while time.time() < end and not os.path.exists(movie):
        time.sleep(0.5)
    time.sleep(1)
    shot("6-exported")
    check("the export is written", os.path.exists(movie), movie)
    if os.path.exists(movie):
        probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                                "stream=codec_name,profile,pix_fmt", "-of", "default=nw=1", movie],
                               capture_output=True, text=True).stdout
        check("ProRes 4444 with an alpha plane", "profile=4444" in probe and "pix_fmt=yuva" in probe, probe.strip())

if __name__ == "__main__":
    {"lower-third": lower_third}[sys.argv[1]]()
    print(f"RESULT: {len(FAILED)} failed" + (": " + ", ".join(FAILED) if FAILED else ""))
