#!/usr/bin/env python3
"""Sets the text of the entry named <name>, over AT-SPI.

    entry.py <name> <text>

For entries that don't report keyboard focus on a display without a window
manager (the effects Browser's search). Reuses
tools/packaging-smoke/drive.py (SMOKE_DRIVE, SMOKE_OUT).
"""
import os
import sys
import time

sys.path.insert(0, os.environ["SMOKE_DRIVE"])
import drive  # noqa: E402

name, text = sys.argv[1], sys.argv[2]
error = "NOT FOUND"
for _ in range(20):
    try:
        for node in drive.walk(drive.app_node()):
            label, role = drive.info(node)
            if label == name and node.get_editable_text_iface():
                node.get_editable_text_iface().set_text_contents(text)
                drive.log(f"entry '{name}' = {text}")
                sys.exit(0)
    except Exception as e:
        error = str(e)
    time.sleep(0.3)
drive.log(f"entry '{name}': {error}")
sys.exit(1)
