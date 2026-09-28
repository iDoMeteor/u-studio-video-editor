#!/usr/bin/env python3
"""Holds a key down, takes a screenshot while it's held, and lets go.

    hold.py <X keycode> <seconds> <screenshot.png>

For the effects "hold \\ to see without effects" (keycode 51 on a US
layout): an application accelerator can't see a release, so the Rack
listens for the key itself. Reuses tools/packaging-smoke/drive.py.
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.environ["SMOKE_DRIVE"])
import drive  # noqa: E402
from gi.repository import Atspi  # noqa: E402

code, seconds, shot = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
Atspi.generate_keyboard_event(code, None, Atspi.KeySynthType.PRESS)
time.sleep(seconds)
subprocess.run(["import", "-window", "root", shot], capture_output=True, timeout=30)
Atspi.generate_keyboard_event(code, None, Atspi.KeySynthType.RELEASE)
time.sleep(0.5)
drive.log(f"held key {code} for {seconds}s")
