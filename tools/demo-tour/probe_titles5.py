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
n = find('{{name}}', roles=('label',), timeout=3); click(*centre_of(n)); pause(1.2)
press('Add In…', roles=TG, exact=True); pause(2); dump('t-addin')
p = find('Pop', roles=('button', 'label', 'list item', 'table cell', 'toggle button', 'radio button'), exact=True, timeout=2)
T.log(f"Pop: {info(p) if p else None} {centre_of(p) if p else None}")
if p: click(*centre_of(p)); pause(1.5)
else: keysym(K_ESC); pause(0.8)
shot('t-pop')
press('Play the intro'); pause(3); shot('t-playing'); press('Play the intro'); pause(0.5)
press('Main menu', roles=TG); pause(1.2); click(1710, 212); pause(2); dump('t-export'); keysym(K_ESC); pause(1)
press('Main menu', roles=TG); pause(1.2); click(1750, 117); pause(2); dump('t-gallery2')
press('Open Pack…'); pause(2); dump('t-openpack'); keysym(K_ESC); pause(1); keysym(K_ESC); pause(1)
keysym(ord('s'), mods=('ctrl',)); pause(2)
press('Close', exact=True); pause(2)
use_app(None)
act('seek-home'); act('step-forward-10', 12, gap=0.1); pause(1.5)
b = clips_in_row(752 + 38); T.log(f"row0 boxes {b[:4]}")
if b: click((b[0][0] + b[0][1]) // 2, 752 + 38); pause(1)
click(*where('Inspector', roles=TG)); pause(2); dump('e-inspector')
quit_app()
