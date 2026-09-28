#!/bin/sh
# Effects smoke test (doc 15, FX2): drives a build's editor with the mouse
# and keyboard only, over AT-SPI, on a private Xvfb display with its own
# D-Bus session and AT-SPI bus, so nothing appears on the desktop and nothing
# reaches running apps. Imports a generated clip, adds an effect from the
# Rack, changes it, undoes, saves, and checks the saved project.
#
#   tools/effects-smoke/run.sh <builddir> <outdir>
#
# The build needs -Ddropin_effects=builtin (or module). Needs Xvfb, python3
# with gi (Atspi), ImageMagick's `import` and ffmpeg. Reuses
# tools/packaging-smoke/drive.py for the individual steps. Exit status 0
# when every check passes; screenshots and logs land in <outdir>.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=$(realpath "${1:?usage: run.sh <builddir> <outdir>}")
OUT=$(realpath -m "${2:?usage: run.sh <builddir> <outdir>}")
mkdir -p "$OUT/home"
APP="$BUILD/src/app/u-studio-video-editor"
[ -x "$APP" ] || { echo "no $APP"; exit 2; }
# Activated services get their own runtime dir: a private
# xdg-document-portal would otherwise mount over the desktop's
# /run/user/<uid>/doc and leave it unmounted on exit.
RUNTIME=$(mktemp -d "${TMPDIR:-/tmp}/uste-rt.XXXXXX")
trap 'fusermount3 -u "$RUNTIME/doc" 2>/dev/null; rm -rf "$RUNTIME"' EXIT
cd "$OUT"
env -i HOME="$OUT/home" USER="$USER" PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 \
    XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" SMOKE_RUNTIME="$RUNTIME" GIO_USE_VFS=local \
    GDK_BACKEND=x11 GDK_DEBUG=no-portals \
    XDG_CONFIG_HOME="$OUT/home/config" XDG_DATA_HOME="$OUT/home/data" XDG_STATE_HOME="$OUT/home/state" \
    XDG_CACHE_HOME="$OUT/home/cache" SMOKE_OUT="$OUT" SMOKE_APP="$APP" SMOKE_HERE="$HERE" \
    SMOKE_DRIVE="$HERE/../packaging-smoke" SMOKE_MEDIA="$OUT/media" \
    dbus-run-session -- sh "$HERE/steps.sh" 2>&1 | tee "$OUT/run.log"
tail -1 "$OUT/run.log" | grep -q "^RESULT: 0 failed"
