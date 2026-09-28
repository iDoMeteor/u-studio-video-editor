#!/usr/bin/env python3
"""Prints the centre of the rightmost curve-lane keyframe dot in a
screenshot: the brand magenta (#ff2bd6), in the timeline's part of the
screen (between y=740 and 980, above the transport bar, and left of
x=1500, where the inspector starts, on the
1920x1080 Xvfb screen; the inspector and transport have magenta too).

    keydot.py <screenshot.png>
"""
import sys

from PIL import Image

image = Image.open(sys.argv[1]).convert("RGB")
width, height = image.size
pixels = image.load()
best = None
for y in range(740, min(height, 980)):
    for x in range(min(width, 1500)):
        if pixels[x, y] == (255, 43, 214):
            if best is None or x > best[0]:
                best = (x, y)
if best is None:
    sys.exit(1)
# The dot is 7 px across: from its rightmost pixel, its centre.
print(f"{best[0] - 3} {best[1]}")
