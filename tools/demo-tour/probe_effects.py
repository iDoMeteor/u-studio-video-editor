# Probe for the effects parts: the Add page (E Browser) and the Effects page (Rack).
import os, sys, time
sys.path.insert(0, os.environ['TOUR_DEMO'])
import tourlib as T
from tourlib import *
M = os.environ['TOUR_MEDIA']; O = os.environ['TOUR_OUT']
def dump(n): dump_tree(os.path.join(O, f'tree-{n}.txt')); shot(n)
def is_on(node):
    st = node.get_state_set()
    return st.contains(Atspi.StateType.PRESSED) or st.contains(Atspi.StateType.CHECKED)
def inspector(open_=False):
    n = find('Inspector', roles=('toggle button',), timeout=4)
    if n and is_on(n) != open_:
        click(*centre_of(n)); pause(0.8)
def import_folder(folder):
    act('import'); center_dialogs(1.0)
    set_location(folder.rstrip('/') + '/'); keysym(K_RETURN); pause(1.5)
    (dx, dy), dw, dh = dialog_origin()
    click(dx + 300, dy + 93); keysym(ord('a'), mods=('ctrl',)); pause(0.8)
    press('Open', exact=True)

launch(); inspector(False)
import_folder(M + '/unicorn'); pause(6)
act('seek-home'); act('step-forward-10', 6, gap=0.05); pause(1)
click(120, 752 + 38); pause(0.8)              # select the first clip on V1
act('effects-browser'); pause(3); dump('fx-add-0')
for t in (30, 60, 90):
    pause(30); shot(f'fx-add-{t}')
dump('fx-add-90')
s = find('Search effects', roles=('text', 'entry'), timeout=2)
T.log(f"search {info(s) if s else None}")
if s:
    click(*centre_of(s)); pause(0.4); typestr('glow'); pause(2.5); dump('fx-search')
tiles = [n for n in walk(app_node()) if info(n)[2] in ('button', 'table cell', 'list item') and extents(n)[0] > 1500 and extents(n)[3] > 60]
T.log(f"tiles: {[info(n)[0][:30] for n in tiles[:12]]}")
if tiles:
    move(*centre_of(tiles[0]), dur=0.8); pause(3); shot('fx-audition')
    click(*centre_of(tiles[0])); pause(3); dump('fx-added')
act('effects-compare'); pause(2.5); dump('fx-compare'); act('effects-compare'); pause(1)
quit_app()
