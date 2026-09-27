import os, sys, time
sys.path.insert(0, os.environ['TOUR_DEMO'])
import tourlib as T
from tourlib import *
from PIL import Image
M = os.environ['TOUR_MEDIA']
def import_folder(folder):
    act('import'); center_dialogs(1.0)
    set_location(folder.rstrip('/') + '/'); keysym(K_RETURN); pause(1.5)
    (dx, dy), dw, dh = dialog_origin()
    click(dx + 300, dy + 93); keysym(ord('a'), mods=('ctrl',)); pause(0.8)
    press('Open', exact=True)
launch()
import_folder(M + '/unicorn'); pause(5)
press('Add track'); pause(1)
import_folder(M + '/zizzle'); pause(5)
act('seek-home'); act('step-forward-10', 6); pause(1.5)
click(960, 385); pause(1.2); shot('tf-selected')
def box(img, x0=5, x1=1915, y0=52, y1=718):
    im = Image.open(img).convert('RGB'); pts=[]
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            p = im.getpixel((x, y))
            if p[2] > 200 and p[1] > 180 and p[0] < 90: pts.append((x, y))
    if not pts: return None
    xs=[p[0] for p in pts]; ys=[p[1] for p in pts]
    return min(xs), min(ys), max(xs), max(ys), len(pts)
T.log(f"box {box(os.path.join(os.environ['TOUR_OUT'], 'tf-selected.png'))} status {status()!r}")
b = box(os.path.join(os.environ['TOUR_OUT'], 'tf-selected.png'))
x0, y0, x1, y1, _ = b
drag(x0 + 1, y0 + 1, x0 + (x1 - x0) // 2, y0 + (y1 - y0) // 2, dur=1.2); pause(1.5); shot('tf-scaled')
b2 = box(os.path.join(os.environ['TOUR_OUT'], 'tf-scaled.png')); T.log(f"after scale {b2}")
cx, cy = (b2[0] + b2[2]) // 2, (b2[1] + b2[3]) // 2
drag(cx, cy, cx + 200, cy - 150, dur=1.2); pause(1.5); shot('tf-moved')
T.log(f"after move {box(os.path.join(os.environ['TOUR_OUT'], 'tf-moved.png'))}")
act('transform-edit'); pause(2); center_dialogs(0.5); shot('tf-dialog')
for n in walk(app_node()):
    nm, ds, rl = info(n)
    if rl in ('spin button', 'text', 'slider', 'label', 'button', 'combo box', 'check box', 'toggle button') and nm and len(nm) < 40: T.log(f"DLG {rl}: {nm!r}")
quit_app()
