#!/usr/bin/env python3
"""Sets the value of the n-th spin button in the editor, over AT-SPI.

    value.py <index> <value>

The Rack's numeric controls are GtkSpinButtons, which AT-SPI exposes with
the Value interface; setting it is what typing a number does. Reuses
tools/packaging-smoke/drive.py to find the app (SMOKE_DRIVE, SMOKE_OUT).
"""
import os
import sys
import time

sys.path.insert(0, os.environ["SMOKE_DRIVE"])
import drive  # noqa: E402

index, value = int(sys.argv[1]), float(sys.argv[2])
error = "NOT FOUND"
for _ in range(20):
    # A fresh walk each try: the Rack rebuilds its cards after some edits,
    # which leaves AT-SPI objects from before it defunct.
    try:
        spins = [n for n in drive.walk(drive.app_node()) if drive.info(n)[1] == "spin button"]
        if len(spins) > index and spins[index].get_value_iface().set_current_value(value):
            drive.log(f"value spin {index} = {value}")
            sys.exit(0)
    except Exception as e:  # a defunct object: try again
        error = str(e)
    time.sleep(0.3)
drive.log(f"value spin {index}: {error}")
sys.exit(1)
