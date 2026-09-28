#!/bin/bash
# run_tour.sh <outdir> [record 0|1]
# Env: TOUR_BIN (the editor binary; default: ../../builddir/src/app/u-studio-video-editor),
#      TOUR_MEDIA (a folder with unicorn/, zizzle/, stills/, finals/; see README.md).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(realpath -m "$1"); REC=${2:-0}
BIN=${TOUR_BIN:-$HERE/../../builddir/src/app/u-studio-video-editor}
MEDIA=${TOUR_MEDIA:?set TOUR_MEDIA to the demo media folder}
rm -rf "$OUT/state" "$OUT/config" "$OUT/cache" "$OUT/data" "$OUT/work" "$OUT/home"
mkdir -p "$OUT"/{state,config,cache,data,work,home}
# Services the private D-Bus session activates (the document portal) get their
# own runtime dir, set by inner.sh as the activation environment: otherwise
# xdg-document-portal mounts over the desktop's /run/user/<uid>/doc and leaves
# it unmounted on exit (2026-09-27). The processes inner.sh starts keep the
# real XDG_RUNTIME_DIR, which the AT-SPI bus needs. Recipe from
# tools/titles-smoke (4ff4065, docs/developer/testing.md).
RUNTIME=$(mktemp -d "${TMPDIR:-/tmp}/ustdemo-rt.XXXXXX")
# The portal can still be unmounting when the session ends; retry briefly.
trap 'for i in 1 2 3 4 5 6; do rm -rf "$RUNTIME" 2>/dev/null && break; sleep 1; done' EXIT
# HOME is the run's own too: an untitled project's New Title falls back to
# $HOME/U Stu Titles (2026-09-28), and nothing may land in the owner's home.
# A display number nobody uses; DISPLAY goes in before the D-Bus session
# starts, so services it activates use the Xvfb too.
N=97; while [ -e "/tmp/.X11-unix/X$N" ]; do N=$((N + 1)); done
cd "$HERE" && timeout 3600 env -i HOME="$OUT/home" USER="$USER" PATH=/usr/bin:/bin LANG=en_US.UTF-8 \
  XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" XDG_STATE_HOME="$OUT/state" XDG_CONFIG_HOME="$OUT/config" \
  XDG_CACHE_HOME="$OUT/cache" XDG_DATA_HOME="$OUT/data" \
  TOUR_RUNTIME="$RUNTIME" GIO_USE_VFS=local DISPLAY=":$N" GDK_BACKEND=x11 GDK_DEBUG=no-portals USTUDIO_LOG_LEVEL=debug \
  XCURSOR_THEME=Adwaita XCURSOR_SIZE=24 XCURSOR_PATH=/usr/share/icons GTK_A11Y=atspi \
  TOUR_GPU="${TOUR_GPU:-1}" TOUR_STOP_AFTER="${TOUR_STOP_AFTER:-}" TOUR_UPTO="${TOUR_UPTO:-99}" TOUR_OUT="$OUT" TOUR_DEMO="$HERE" TOUR_BIN="$BIN" TOUR_MEDIA="$MEDIA" \
  dbus-run-session -- bash "$HERE/inner.sh" "${TOUR_SCRIPT:-$HERE/tour.py}" "$OUT" "$REC" > "$OUT/run.log" 2>&1
echo "rc $?"
