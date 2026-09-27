#!/bin/sh
# Titles designer smoke test (doc 16, T2 acceptance): designs a lower third
# from a blank canvas with the mouse and keyboard only, saves it, and checks
# the saved title. On a private Xvfb display with its own D-Bus session and
# AT-SPI bus, so nothing appears on the desktop and nothing reaches the
# user's running apps.
#
#   tools/titles-smoke/run.sh <builddir> <outdir>
#
# Then exports it with alpha (ProRes 4444) through the Export dialog and
# checks the file with ffprobe. Needs Xvfb, python3 with gi (Atspi) and
# python-xlib, ImageMagick's `import`, ffprobe, the AT-SPI bus, and the
# build's u-studio-render. Exit status 0 when every check passes.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=$(realpath "${1:?usage: run.sh <builddir> <outdir>}")
OUT=$(realpath -m "${2:?usage: run.sh <builddir> <outdir>}")
mkdir -p "$OUT/home"
# The services the private D-Bus session activates get their own runtime
# dir (steps.sh sets it as the activation environment): its
# xdg-document-portal would otherwise mount over the desktop's
# /run/user/<uid>/doc and leave it unmounted on exit (2026-09-27). The
# processes steps.sh starts keep the real one, which AT-SPI needs.
RUNTIME=$(mktemp -d "${TMPDIR:-/tmp}/ust-rt.XXXXXX")
trap 'rm -rf "$RUNTIME"' EXIT
APP="$BUILD/drop-ins/titles/app/u-studio-titles"
[ -x "$APP" ] || { echo "no $APP: build with -Ddropin_titles=builtin or module"; exit 2; }
# A display number nobody uses; DISPLAY and GDK_BACKEND go in before the
# D-Bus session starts, so services it activates use the Xvfb too, and
# GDK_DEBUG=no-portals keeps dialogs in the app (reachable over AT-SPI).
N=90; while [ -e "/tmp/.X11-unix/X$N" ]; do N=$((N + 1)); done
cd "$OUT"
env -i HOME="$OUT/home" USER="$USER" PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 \
    XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" SMOKE_RUNTIME="$RUNTIME" GIO_USE_VFS=local \
    DISPLAY=":$N" GDK_BACKEND=x11 GDK_DEBUG=no-portals \
    XDG_CONFIG_HOME="$OUT/home/config" XDG_DATA_HOME="$OUT/home/data" XDG_STATE_HOME="$OUT/home/state" \
    XDG_CACHE_HOME="$OUT/home/cache" SMOKE_OUT="$OUT" SMOKE_APP="$APP" SMOKE_HERE="$HERE" \
    dbus-run-session -- sh "$HERE/steps.sh" 2>&1 | tee "$OUT/run.log"
tail -1 "$OUT/run.log" | grep -q "^RESULT: 0 failed"
