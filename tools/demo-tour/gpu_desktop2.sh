#!/bin/bash
# GPU acceleration on the real desktop, one window: the prepared project plays
# 10 s with GPU acceleration on, then GPU acceleration is switched off in
# Settings (AT-SPI actions, no pointer) and it plays 10 s again; each measured.
# Private D-Bus + a11y bus, portal-safe, silent, a copy of the prep settings.
# It writes <out>/APPUP once the window is up and waits for <ready> (the
# window share) before playing.
#   gpu_desktop2.sh <prep run dir> <out dir> <bin> <ready file>
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
PREP=$1; OUT=$(realpath -m "$2"); BIN=$3; READYF=$4
mkdir -p "$OUT"
for d in config state data cache home; do cp -a "$PREP/$d" "$OUT/$d"; done
RUNTIME=$(mktemp -d "${TMPDIR:-/tmp}/ustdemo-rt.XXXXXX")
trap 'for i in 1 2 3 4 5 6; do rm -rf "$RUNTIME" 2>/dev/null && break; sleep 1; done' EXIT
env -i HOME="$OUT/home" USER="$USER" PATH=/usr/bin:/bin LANG=en_US.UTF-8 \
  XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}" \
  XDG_CONFIG_HOME="$OUT/config" XDG_STATE_HOME="$OUT/state" XDG_DATA_HOME="$OUT/data" XDG_CACHE_HOME="$OUT/cache" \
  GIO_USE_VFS=local GDK_DEBUG=no-portals GTK_A11Y=atspi SDL_AUDIODRIVER=dummy SDL_AUDIO_DRIVER=dummy USTUDIO_LOG_LEVEL=debug \
  RUNTIME="$RUNTIME" OUT="$OUT" BIN="$BIN" HERE="$HERE" READYF="$READYF" \
  dbus-run-session -- bash -c '
    dbus-update-activation-environment XDG_RUNTIME_DIR="$RUNTIME" GIO_USE_VFS=local
    /usr/libexec/at-spi-bus-launcher --launch-immediately >/dev/null 2>&1 & L=$!
    for i in $(seq 1 40); do gdbus call --session --dest org.a11y.Bus --object-path /org/a11y/bus --method org.a11y.Bus.GetAddress >/dev/null 2>&1 && break; sleep 0.25; done
    /usr/libexec/at-spi2-registryd >/dev/null 2>&1 & R=$!
    act() { gdbus call --session --dest com.ustudio.VideoEditor --object-path /com/ustudio/VideoEditor/window/1 \
              --method org.gtk.Actions.Activate "$1" "[]" "{}" >/dev/null 2>&1; }
    cpu() { awk "{print \$14 + \$15}" /proc/$1/stat; }
    mark() { echo "$(date +%s.%N) $1" >> "$OUT/marks.txt"; }
    measure() {
      act seek-home; sleep 0.3
      for i in 1 2 3 4 5 6 7 8 9 10; do act step-forward; done; sleep 2
      c0=$(cpu $APP); t0=$(date +%s.%N); mark "play $1"
      act play-pause; sleep 10; act play-pause
      c1=$(cpu $APP); t1=$(date +%s.%N); mark "pause $1"
      sleep 1.5
      f=$(grep -o "pause() at frame [0-9]*" "$OUT/app.log" | tail -1 | grep -o "[0-9]*$")
      python3 -c "import sys; c0,c1,t0,t1,f=map(float,sys.argv[1:]); print(f\"$1: cpu {(c1-c0)/(t1-t0):.0f}% fps {(f-10)/(t1-t0):.1f}\")" $c0 $c1 $t0 $t1 $f | tee -a "$OUT/result.txt"
    }
    dconf write /com/ustudio/VideoEditor/gpu-acceleration true
    dconf write /com/ustudio/VideoEditor/default-preview-scale "\"full\""
    mark launch
    "$BIN" > "$OUT/app.log" 2>&1 & APP=$!
    for i in $(seq 1 60); do act seek-home && break; sleep 0.5; done
    sleep 4; touch "$OUT/APPUP"; mark appup
    for i in $(seq 1 300); do [ -f "$READYF" ] && break; sleep 1; done
    if [ -f "$READYF" ]; then
      sleep 2; measure on
      mark "settings"; python3 "$HERE/gpu_toggle.py" open; sleep 2.5
      python3 "$HERE/gpu_toggle.py" toggle; mark "toggled"; sleep 2
      python3 "$HERE/gpu_toggle.py" close; sleep 2
      measure off
      grep "\[gpu\] GPU pipeline" "$OUT/app.log" >> "$OUT/result.txt"
    fi
    kill -TERM $APP; for i in $(seq 1 40); do kill -0 $APP 2>/dev/null || break; sleep 0.25; done
    kill -0 $APP 2>/dev/null && kill -KILL $APP
    mark closed
    kill $R $L 2>/dev/null
  ' > "$OUT/session.log" 2>&1
echo done
