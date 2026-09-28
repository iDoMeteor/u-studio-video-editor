# Probe for writing the titles chapters: dumps the editor's and U Stu Titles'
# AT-SPI trees (role, name, box) with screenshots. Run as TOUR_SCRIPT.
import os, sys, subprocess, time
sys.path.insert(0, os.environ['TOUR_DEMO'])
import tourlib as T
from tourlib import *
M = os.environ['TOUR_MEDIA']
O = os.environ['TOUR_OUT']

def import_folder(folder):
    act('import'); center_dialogs(1.0)
    set_location(folder.rstrip('/') + '/'); keysym(K_RETURN); pause(1.5)
    (dx, dy), dw, dh = dialog_origin()
    click(dx + 300, dy + 93); keysym(ord('a'), mods=('ctrl',)); pause(0.8)
    press('Open', exact=True)

launch()
shot('p0-start'); dump_tree(os.path.join(O, 'tree-editor-start.txt'))
import_folder(M + '/unicorn'); pause(6)
press('Add track'); pause(1.2)
act('seek-home'); act('step-forward-10', 9, gap=0.1); pause(1)
act('titles-new'); pause(6)
shot('p1-newtitle-editor')
use_app('u-studio-titles')
for _ in range(40):
    if app_node(): break
    time.sleep(0.5)
T.log(f"titles app: {bool(app_node())}")
subprocess.run(['python3', os.path.join(T.DEMO, 'fitwin.py'), 'u-studio-titles'], capture_output=True, timeout=30)
pause(2); shot('p2-gallery'); dump_tree(os.path.join(O, 'tree-titles-gallery.txt'))
use_app(None)
dump_tree(os.path.join(O, 'tree-editor-title.txt'))
quit_app()
