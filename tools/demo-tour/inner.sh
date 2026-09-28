#!/bin/bash
# Runs inside dbus-run-session: Xvfb, a11y bus, optional recorders, then the tour.
set -u
SCRIPT=$1; OUT=$2; RECORD=${3:-0}
# Activated services (portals) run in run_tour.sh's private runtime dir.
dbus-update-activation-environment XDG_RUNTIME_DIR="$TOUR_RUNTIME" GIO_USE_VFS=local
Xvfb "$DISPLAY" -screen 0 1920x1080x24 -nolisten tcp >/dev/null 2>&1 & XV=$!
sleep 1
/usr/libexec/at-spi-bus-launcher --launch-immediately >/dev/null 2>&1 & L=$!
for i in $(seq 1 40); do gdbus call --session --dest org.a11y.Bus --object-path /org/a11y/bus --method org.a11y.Bus.GetAddress >/dev/null 2>&1 && break; sleep 0.25; done
/usr/libexec/at-spi2-registryd >/dev/null 2>&1 & R=$!
sleep 1
export TOUR_T0=$(date +%s.%N)
FF=; AR=
if [ "$RECORD" = 1 ]; then
  rm -f $OUT/audio.fifo $OUT/audio.fifo.stop; mkfifo $OUT/audio.fifo
  python3 $TOUR_DEMO/audiorec.py $OUT/audio.fifo $OUT/audio.raw $TOUR_T0 & AR=$!
  ffmpeg -loglevel error -y -f x11grab -framerate 30 -video_size 1920x1080 -draw_mouse 1 -i "$DISPLAY" \
    -c:v libx264 -preset ultrafast -crf 18 $OUT/screen.mkv & FF=$!
  export SDL_AUDIODRIVER=disk SDL_AUDIO_DRIVER=disk SDL_DISKAUDIOFILE=$OUT/audio.fifo SDL_AUDIO_DISK_OUTPUT_FILE=$OUT/audio.fifo
else
  export SDL_AUDIODRIVER=dummy SDL_AUDIO_DRIVER=dummy
fi
python3 $SCRIPT
RC=$?
if [ -n "$FF" ]; then kill -INT $FF; wait $FF 2>/dev/null; fi
if [ -n "$AR" ]; then touch $OUT/audio.fifo.stop; (exec 3>$OUT/audio.fifo; exec 3>&-) ; sleep 0.5; kill $AR 2>/dev/null; fi
kill $R $L $XV 2>/dev/null
exit $RC
