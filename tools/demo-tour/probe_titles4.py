# Probe 2 for the titles chapters: template -> designer tree and menu -> save -> editor inspector.
import os, sys, subprocess, time
sys.path.insert(0, os.environ['TOUR_DEMO'])
import tourlib as T
from tourlib import *
M = os.environ['TOUR_MEDIA']; O = os.environ['TOUR_OUT']
def import_folder(folder):
    act('import'); center_dialogs(1.0)
    set_location(folder.rstrip('/') + '/'); keysym(K_RETURN); pause(1.5)
    (dx, dy), dw, dh = dialog_origin()
    click(dx + 300, dy + 93); keysym(ord('a'), mods=('ctrl',)); pause(0.8)
    press('Open', exact=True)
def dump(name): dump_tree(os.path.join(O, f'tree-{name}.txt')); shot(name)
launch()
import_folder(M + '/unicorn'); pause(6)
T.log(f"Import at {where('Import…')}; Save at {where('Save project')}; Render at {where('Render…')}")
move(*where('Save project')); pause(1.5); shot('e-hover-save')   # the tooltip shows where the pointer landed
press('Add track'); pause(1.2)
click(1200, 752 + 38); pause(0.8)   # empty top track: make it active
act('seek-home'); act('step-forward-10', 9, gap=0.1); pause(1)
act('titles-new'); pause(6)
use_app('u-studio-titles')
for _ in range(40):
    if app_node(): break
    time.sleep(0.5)
subprocess.run(['python3', os.path.join(T.DEMO, 'fitwin.py'), 'u-studio-titles'], capture_output=True, timeout=30)
pause(2)
press('Lower third, two lines', exact=True); pause(3); dump('t-edit')
TG = ('toggle button',)
press('Main menu', roles=TG); pause(1.5); dump('t-menu'); keysym(K_ESC); pause(0.8)
press('Add a layer', roles=TG); pause(1.5); dump('t-add'); keysym(K_ESC); pause(0.8)
n = find('{{name}}', roles=('label', 'list item', 'table cell', 'row'), timeout=3)
T.log(f"name layer node {info(n) if n else None} at {centre_of(n) if n else None}")
if n: click(*centre_of(n)); pause(1.5)
dump('t-layer')
if press_any('Add In'): pause(2); dump('t-addin'); keysym(K_ESC); pause(0.8)
keysym(ord('s'), mods=('ctrl',)); pause(2)
press('Close', exact=True); pause(2)
use_app(None)
shot('e-after-close')
click(*where('Inspector', roles=TG)); pause(1)
row0 = 752 + 38
click(560, row0); pause(1.5); dump('e-inspector')
quit_app()
