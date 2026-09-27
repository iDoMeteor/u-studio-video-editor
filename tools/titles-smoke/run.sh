#!/bin/sh
# Titles designer smoke test (doc 16, T2 acceptance): designs a lower third
# from a blank canvas with the mouse and keyboard only, saves it, and checks
# the saved title. On a private Xvfb display with its own D-Bus session and
# AT-SPI bus, so nothing appears on the desktop and nothing reaches the
# user's running apps.
#
#   tools/titles-smoke/run.sh <builddir> <outdir>
#
# Needs Xvfb, python3 with gi (Atspi) and python-xlib, ImageMagick's
# `import`, and the AT-SPI bus. Exit status 0 when every check passes.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=$(realpath "${1:?usage: run.sh <builddir> <outdir>}")
OUT=$(realpath -m "${2:?usage: run.sh <builddir> <outdir>}")
mkdir -p "$OUT/home"
APP="$BUILD/drop-ins/titles/app/u-studio-titles"
[ -x "$APP" ] || { echo "no $APP: build with -Ddropin_titles=builtin or module"; exit 2; }
# A display number nobody uses; DISPLAY and GDK_BACKEND go in before the
# D-Bus session starts, so services it activates use the Xvfb too, and
# GDK_DEBUG=no-portals keeps dialogs in the app (reachable over AT-SPI).
N=90; while [ -e "/tmp/.X11-unix/X$N" ]; do N=$((N + 1)); done
cd "$OUT"
env -i HOME="$OUT/home" USER="$USER" PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 \
    XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" DISPLAY=":$N" GDK_BACKEND=x11 GDK_DEBUG=no-portals \
    XDG_CONFIG_HOME="$OUT/home/config" XDG_DATA_HOME="$OUT/home/data" XDG_STATE_HOME="$OUT/home/state" \
    XDG_CACHE_HOME="$OUT/home/cache" SMOKE_OUT="$OUT" SMOKE_APP="$APP" SMOKE_HERE="$HERE" \
    dbus-run-session -- sh "$HERE/steps.sh" 2>&1 | tee "$OUT/run.log"
tail -1 "$OUT/run.log" | grep -q "^RESULT: 0 failed"
