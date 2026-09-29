import os, sys, subprocess, time
sys.path.insert(0, os.environ['TOUR_DEMO'])
import tourlib as T
from tourlib import *
launch(); pause(3)
r = subprocess.run(['python3', os.path.join(T.DEMO, 'gpu_toggle.py'), 'all'], capture_output=True, text=True, timeout=60)
T.log('toggle: ' + r.stdout.replace('\n', ' | ') + r.stderr[-300:])
shot('after-toggle'); pause(1)
quit_app()
