#!/bin/sh
# In MLT XML, a producer whose resource is "color:red" (or "colour:…") loads black.
# producer_xml creates the producer from "color:red" (the loader strips the prefix, so
# the producer's resource is "red"), then copies the XML's properties onto it, which
# sets resource back to "color:red", a string producer_colour can't parse.
# Expected: every line red (255 0 0). Actual (MLT 7.40.0): the "color:" lines are black.
# Needs melt and ffmpeg. Run it in a scratch directory.
set -e
MELT=${MELT:-melt}
try() {
    cat > t.mlt <<XML
<?xml version="1.0"?>
<mlt>
  <profile width="64" height="36" frame_rate_num="25" frame_rate_den="1" progressive="1"
           sample_aspect_num="1" sample_aspect_den="1" display_aspect_num="16" display_aspect_den="9"/>
  <producer id="c">$1<property name="length">10</property></producer>
  <playlist id="p"><entry producer="c" in="0" out="0"/></playlist>
</mlt>
XML
    rm -f t.png
    $MELT -quiet t.mlt -consumer avformat:t.png vcodec=png pix_fmt=rgb24
    echo "$(ffmpeg -v quiet -i t.png -f rawvideo -pix_fmt rgb24 - | od -An -tu1 -N3)  <- $1"
}
try '<property name="resource">color:red</property>'
try '<property name="resource">color:0xff0000ff</property>'
try '<property name="mlt_service">color</property><property name="resource">color:red</property>'
try '<property name="resource">0xff0000ff</property>'
try '<property name="mlt_service">color</property><property name="resource">red</property>'
