#!/bin/bash
# GPU acceleration on the real desktop: the prepared three-track project
# (gpu_prep.py), played for 10 s at Full with GPU acceleration on, then again
# with it off, each measured (the editor's CPU from /proc; frames played from
# its debug log). A private D-Bus session (so it can't reach the owner's
# running editor), portal-safe (docs/developer/testing.md), silent audio, and
# a copy of the prep run's settings, so nothing of the owner's changes.
#   gpu_desktop.sh <prep run dir> <out dir> <bin>
set -u
PREP=$1; OUT=$(realpath -m "$2"); BIN=$3
mkdir -p "$OUT"
for d in config state data cache home; do cp -a "$PREP/$d" "$OUT/$d"; done
RUNTIME=$(mktemp -d "${TMPDIR:-/tmp}/ustdemo-rt.XXXXXX")
trap 'for i in 1 2 3 4 5 6; do rm -rf "$RUNTIME" 2>/dev/null && break; sleep 1; done' EXIT
env -i HOME="$OUT/home" USER="$USER" PATH=/usr/bin:/bin LANG=en_US.UTF-8 \
  XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}" \
  XDG_CONFIG_HOME="$OUT/config" XDG_STATE_HOME="$OUT/state" XDG_DATA_HOME="$OUT/data" XDG_CACHE_HOME="$OUT/cache" \
  GIO_USE_VFS=local GDK_DEBUG=no-portals SDL_AUDIODRIVER=dummy SDL_AUDIO_DRIVER=dummy USTUDIO_LOG_LEVEL=debug \
  RUNTIME="$RUNTIME" OUT="$OUT" BIN="$BIN" \
  dbus-run-session -- bash -c '
    dbus-update-activation-environment XDG_RUNTIME_DIR="$RUNTIME" GIO_USE_VFS=local
    act() { gdbus call --session --dest com.ustudio.VideoEditor --object-path /com/ustudio/VideoEditor/window/1 \
              --method org.gtk.Actions.Activate "$1" "[]" "{}" >/dev/null 2>&1; }
    cpu() { awk "{print \$14 + \$15}" /proc/$1/stat; }
    for mode in on off; do
      if [ $mode = on ]; then v=true; else v=false; fi
      dconf write /com/ustudio/VideoEditor/gpu-acceleration $v
      dconf write /com/ustudio/VideoEditor/default-preview-scale "\"full\""
      echo "$(date +%s.%N) launch $mode" >> "$OUT/marks.txt"
      "$BIN" > "$OUT/app-$mode.log" 2>&1 & APP=$!
      for i in $(seq 1 60); do act seek-home && break; sleep 0.5; done
      sleep 6
      act seek-home; sleep 0.3
      for i in 1 2 3 4 5 6 7 8 9 10; do act step-forward; done; sleep 2
      c0=$(cpu $APP); t0=$(date +%s.%N)
      echo "$t0 play $mode" >> "$OUT/marks.txt"
      act play-pause; sleep 10; act play-pause
      c1=$(cpu $APP); t1=$(date +%s.%N)
      echo "$t1 pause $mode" >> "$OUT/marks.txt"
      sleep 1.5
      f=$(grep -o "pause() at frame [0-9]*" "$OUT/app-$mode.log" | tail -1 | grep -o "[0-9]*$")
      python3 -c "import sys; c0,c1,t0,t1,f=map(float,sys.argv[1:]); tk=100; print(f\"$mode: cpu {100*(c1-c0)/tk/(t1-t0):.0f}% fps {(f-10)/(t1-t0):.1f}\")" $c0 $c1 $t0 $t1 $f | tee -a "$OUT/result.txt"
      grep -m1 "\[gpu\] GPU pipeline" "$OUT/app-$mode.log" >> "$OUT/result.txt"
      kill -TERM $APP; for i in $(seq 1 40); do kill -0 $APP 2>/dev/null || break; sleep 0.25; done
      kill -0 $APP 2>/dev/null && kill -KILL $APP
      echo "$(date +%s.%N) closed $mode" >> "$OUT/marks.txt"
      sleep 1
    done
  '
echo done
