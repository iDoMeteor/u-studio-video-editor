#!/bin/sh
# movit.luma_mix squares the transition's progress (transition_movit_luma.cpp,
# "Make progress appear linear using a gamma curve"). For a plain dissolve in linear
# light that is deliberate, but with a luma map it also bends the wipe's timing: the
# edge stays put for the first frames and then catches up, unlike the CPU luma.
# A horizontal 16-bit PGM ramp wipes blue into green over 20 frames at 1080p30.
# Actual (MLT 7.40.0, movit 1.7.1, Mesa on Intel Iris Xe), edge x per frame:
#   luma            0   10  116  220  326  430  538  643 ... 1906
#   movit.luma_mix  0    0    0    0   53  254  414  554 ... 1880
# melt runs the movit case through its qglsl consumer, which needs a GL-capable Qt
# platform (a Wayland or X11 session; QT_QPA_PLATFORM=offscreen has no GL).
# Needs melt, python3 and ffmpeg. Run it in a scratch directory.
set -e
MELT=${MELT:-melt}
python3 - <<'PY'
import struct
w, h = 640, 360
with open("wipe16.pgm", "wb") as f:
    f.write(b"P5\n%d %d\n65535\n" % (w, h))
    f.write(b"".join(struct.pack(">H", int(65535 * x / (w - 1))) for x in range(w)) * h)
PY
for c in 2040c0:blue 20c040:green; do
    ffmpeg -v error -y -f lavfi -i color=c=0x${c%%:*}:s=1920x1080:r=30 -t 3 -c:v libx264 \
        -pix_fmt yuv420p -colorspace bt709 -color_primaries bt709 -color_trc bt709 ${c##*:}.mp4
done
for t in luma movit.luma_mix; do
    mkdir -p "$t"
    $MELT -quiet -profile atsc_1080p_30 blue.mp4 in=40 out=59 -track green.mp4 in=0 out=19 \
        -transition $t resource=wipe16.pgm softness=0.1 in=0 out=19 a_track=0 b_track=1 \
        -consumer avformat:$t/f%02d.png vcodec=png pix_fmt=rgb24
done
# The edge: the first x on row 540 whose green falls below 128 (blue ~62, green ~190).
python3 - <<'PY'
import subprocess
for t in ("luma", "movit.luma_mix"):
    xs = []
    for f in range(1, 21):
        raw = subprocess.check_output(["ffmpeg", "-v", "error", "-i", f"{t}/f{f:02d}.png",
                                       "-f", "rawvideo", "-pix_fmt", "rgb24", "-"])
        row = raw[540 * 1920 * 3:541 * 1920 * 3]
        xs.append(next((x for x in range(1920) if row[3 * x + 1] < 128), -1))
    print(f"{t:15}", " ".join(f"{x:4d}" for x in xs))
PY
