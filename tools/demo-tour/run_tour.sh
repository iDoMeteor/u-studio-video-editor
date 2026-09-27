#!/bin/bash
# run_tour.sh <outdir> [record 0|1]
# Env: TOUR_BIN (the editor binary; default: ../../builddir/src/app/u-studio-video-editor),
#      TOUR_MEDIA (a folder with unicorn/, zizzle/, stills/, finals/; see README.md).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(realpath -m "$1"); REC=${2:-0}
BIN=${TOUR_BIN:-$HERE/../../builddir/src/app/u-studio-video-editor}
MEDIA=${TOUR_MEDIA:?set TOUR_MEDIA to the demo media folder}
rm -rf "$OUT/state" "$OUT/config" "$OUT/cache" "$OUT/data" "$OUT/work"
mkdir -p "$OUT"/{state,config,cache,data,work}
cd "$HERE" && timeout 3600 env -i HOME="$HOME" USER="$USER" PATH=/usr/bin:/bin LANG=en_US.UTF-8 \
  XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" XDG_STATE_HOME="$OUT/state" XDG_CONFIG_HOME="$OUT/config" \
  XDG_CACHE_HOME="$OUT/cache" XDG_DATA_HOME="$OUT/data" \
  GDK_BACKEND=x11 GDK_DEBUG=no-portals USTUDIO_LOG_LEVEL=debug \
  XCURSOR_THEME=Adwaita XCURSOR_SIZE=24 XCURSOR_PATH=/usr/share/icons GTK_A11Y=atspi \
  TOUR_UPTO="${TOUR_UPTO:-99}" TOUR_OUT="$OUT" TOUR_DEMO="$HERE" TOUR_BIN="$BIN" TOUR_MEDIA="$MEDIA" \
  dbus-run-session -- bash "$HERE/inner.sh" "${TOUR_SCRIPT:-$HERE/tour.py}" "$OUT" "$REC" > "$OUT/run.log" 2>&1
echo "rc $?"
