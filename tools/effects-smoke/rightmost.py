#!/usr/bin/env python3
"""Prints the rightmost pixel of one colour in a band of a screenshot, as
"x y": a curve lane's keyframe dot (the brand magenta), a selected
adjustment block's right edge (its cyan outline). Only the timeline's part
of the 1920x1080 Xvfb screen is searched by default, since the inspector and
the transport bar use the same colours.

    rightmost.py <screenshot.png> <#rrggbb> [ymin ymax [xmax [run [square]]]]

Default band: y 740-980, x below 1500. With `run`, only pixels in a
horizontal run of at least that many of the colour count (a block's
outline, not the playhead's vertical line of the same cyan). With
`square` = n, only the bottom-right pixel of a solid n x n square counts
(a handle, not an outline).
"""
import sys

from PIL import Image

path, colour = sys.argv[1], sys.argv[2].lstrip("#")
target = tuple(int(colour[i:i + 2], 16) for i in (0, 2, 4))
ymin, ymax = (int(sys.argv[3]), int(sys.argv[4])) if len(sys.argv) > 4 else (740, 980)
xmax = int(sys.argv[5]) if len(sys.argv) > 5 else 1500
run = int(sys.argv[6]) if len(sys.argv) > 6 else 1
square = int(sys.argv[7]) if len(sys.argv) > 7 else 0
image = Image.open(path).convert("RGB")
width, height = image.size
pixels = image.load()
best = None
for y in range(ymin, min(ymax, height)):
    length = 0
    for x in range(min(width, xmax)):
        length = length + 1 if pixels[x, y] == target else 0
        if length < run or (best is not None and x <= best[0]):
            continue
        if square and not all(pixels[x - i, y - j] == target for i in range(square) for j in range(square)
                              if x - i >= 0 and y - j >= 0):
            continue
        best = (x, y)
if best is None:
    sys.exit(1)
print(f"{best[0]} {best[1]}")
