#!/bin/sh
# composite blends 4:2:2 YUV with each pixel's own alpha, so the two pixels of a
# pair mix U and V by different amounts where their alphas differ.
# Input: a 1920x1080 white PNG whose alpha alternates per column (255, 5, 255, 5, ...).
# Expected over red (what the affine transition gives): (255,255,255) (255,5,5) ...
# Actual with composite (MLT 7.40.0):                    (255,166,255) (255,0,79) ...
# Needs melt, python3 and ffmpeg. Run it in a scratch directory.
set -e
MELT=${MELT:-melt}
python3 - <<'PY'
import struct, zlib
w, h = 1920, 1080
row = bytearray([0])
for x in range(w):
    row += bytes([255, 255, 255, 255 if x % 2 == 0 else 5])
def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
open("stripes.png", "wb").write(b"\x89PNG\r\n\x1a\n"
    + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    + chunk(b"IDAT", zlib.compress(bytes(row) * h)) + chunk(b"IEND", b""))
PY
show() {
    ffmpeg -v error -i "$1" -f rawvideo -pix_fmt rgb24 - | python3 -c "
import sys; d = sys.stdin.buffer.read(); o = (540 * 1920 + 100) * 3
print('$2', [tuple(d[o + 3 * i:o + 3 * i + 3]) for i in range(4)])"
}
for t in composite affine; do
    $MELT -quiet -profile atsc_1080p_25 color:red out=0 -track stripes.png out=0 \
        -transition $t a_track=0 b_track=1 fill=1 \
        -consumer avformat:$t.png vcodec=png pix_fmt=rgb24
    show $t.png "$t, x=100..103 on row 540:"
done
