#!/bin/sh
# The avformat consumer drops alpha for a yuva pix_fmt unless mlt_image_format=rgba is
# set: consumer_avformat.c picks mlt_image_rgba only for rgba/argb/bgra and otherwise
# asks for yuv422, so a yuva encoder gets an opaque picture.
# Input: a white PNG whose alpha alternates per column (255, 5, ...).
# Expected: alpha 255 5 255 5 in both files, as with mlt_image_format=rgba.
# Actual (MLT 7.40.0): alpha 255 255 255 255 without it.
# Needs melt, python3 and ffmpeg (prores_ks, libvpx-vp9). Run it in a scratch directory.
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
alpha() {
    ffmpeg -v error -c:v "$2" -i "$1" -frames:v 1 -f rawvideo -pix_fmt rgba - | python3 -c "
import sys; d = sys.stdin.buffer.read(); o = (540 * 1920 + 100) * 4
print([d[o + 4 * i + 3] for i in range(4)])"
}
for extra in "" "mlt_image_format=rgba"; do
    rm -f a.mov b.webm
    $MELT -quiet -profile atsc_1080p_25 stripes.png out=0 \
        -consumer avformat:a.mov vcodec=prores_ks pix_fmt=yuva444p10le an=1 $extra
    $MELT -quiet -profile atsc_1080p_25 stripes.png out=0 \
        -consumer avformat:b.webm vcodec=libvpx-vp9 pix_fmt=yuva420p an=1 $extra
    echo "[${extra:-no mlt_image_format}] prores_ks yuva444p10le alpha: $(alpha a.mov prores)"
    echo "[${extra:-no mlt_image_format}] libvpx-vp9 yuva420p alpha:   $(alpha b.webm libvpx-vp9)"
done
