#!/bin/sh
# producer_colour tags its YUV frames BT.601 whatever the profile's colorspace, and
# composite keeps the A frame's tag without converting the B frame. A BT.709 source
# composited over color:black is then converted to RGB with the BT.601 matrix.
# Expected: pure red (BT.709 source, BT.709 profile) reads the same over black as alone.
# Actual (MLT 7.40.0): alone 253,0,0; over color:black 231,0,1; over an RGBA black 253,0,0.
# Needs melt and ffmpeg. Run it in a scratch directory.
set -e
MELT=${MELT:-melt}
ffmpeg -v error -y -f lavfi -i color=c=red:s=320x180:r=25:d=1 \
    -vf scale=out_color_matrix=bt709:out_range=tv,format=yuv420p \
    -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv \
    -c:v libx264 -qp 0 red709.mp4
px() { ffmpeg -v quiet -i "$1" -f rawvideo -pix_fmt rgb24 - | od -An -tu1 -N3; }
P="-profile atsc_1080p_25"
C="-consumer avformat:out.png vcodec=png pix_fmt=rgb24"
$MELT -quiet $P red709.mp4 out=0 $C
echo "alone:                   $(px out.png)"
$MELT -quiet $P color:black out=0 -track red709.mp4 out=0 -transition composite a_track=0 b_track=1 fill=1 $C
echo "over color:black:        $(px out.png)"
$MELT -quiet $P color:black out=0 mlt_image_format=rgba -track red709.mp4 out=0 \
    -transition composite a_track=0 b_track=1 fill=1 $C
echo "over color:black (rgba): $(px out.png)"
