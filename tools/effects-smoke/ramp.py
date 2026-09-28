#!/usr/bin/env python3
"""Moves the n-th spin button through a ramp of values, quickly, over AT-SPI,
as a hand dragging a slider would (touch-record needs the changes close
together; value.py looks the control up afresh each time, which is slower
than a gesture's gap).

    ramp.py <index> <from> <to> <steps> <interval seconds>

Reuses tools/packaging-smoke/drive.py (SMOKE_DRIVE, SMOKE_OUT).
"""
import os
import sys
import time

sys.path.insert(0, os.environ["SMOKE_DRIVE"])
import drive  # noqa: E402

index, start, end = int(sys.argv[1]), float(sys.argv[2]), float(sys.argv[3])
steps, interval = int(sys.argv[4]), float(sys.argv[5])
spins = [n for n in drive.walk(drive.app_node()) if drive.info(n)[1] == "spin button"]
if len(spins) <= index:
    drive.log(f"ramp spin {index}: NOT FOUND")
    sys.exit(1)
value = spins[index].get_value_iface()
for i in range(steps):
    value.set_current_value(start + (end - start) * i / max(steps - 1, 1))
    time.sleep(interval)
drive.log(f"ramp spin {index}: {start} -> {end} in {steps} steps")
