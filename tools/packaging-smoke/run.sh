#!/bin/sh
# Packaging smoke test: drives the installed Flatpak or Snap end to end.
#
#   tools/packaging-smoke/run.sh <flatpak|snap> <outdir> [--cleanup]
#
# The installed package must already be there (flatpak install --user ... or
# snap install --dangerous ...). Everything runs on a private Xvfb display
# with its own D-Bus session and a clean environment, so nothing appears on
# the desktop. Audio goes to SDL's disk driver, so nothing plays on the
# speakers. Needs Xvfb, ffmpeg, python3 with gi (Atspi) and python-xlib,
# ImageMagick's `import`, and the AT-SPI bus (at-spi-bus-launcher,
# at-spi2-registryd).
#
# Flatpak runs are isolated from the user's own state: the app runs with
# HOME set to a throwaway directory ($SMOKE_HOME, default
# ~/.cache/ustudio-smoke-home) and FLATPAK_USER_DIR pointing at the real
# installation, so its ~/.var/app/<id> settings, logs, autosaves and proxies,
# and the test media and projects, all live under that directory. --cleanup
# deletes it afterwards.
#
# Snap runs can't be isolated that way (snapd takes the home directory from
# the passwd database, not $HOME): they use the real ~/snap/<name> data, and
# the media goes to ~/ustudio-smoke-media (not a hidden folder: the home plug
# can't read those). --cleanup deletes both, so only use it for a snap
# nobody else uses on this machine.
#
# Screenshots, logs and the results summary land in <outdir>. Exit status 0
# means every check passed.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
RUNNER=${1:?usage: run.sh <flatpak|snap> <outdir> [--cleanup]}
OUT=$(realpath -m "${2:?usage: run.sh <flatpak|snap> <outdir> [--cleanup]}")
CLEANUP=${3:-}
case "$RUNNER" in flatpak|snap) ;; *) echo "runner must be flatpak or snap" >&2; exit 2 ;; esac
mkdir -p "$OUT"
REAL_HOME=$HOME
APP_ID=${SMOKE_APP_ID:-com.ustudio.VideoEditor}
SNAP_NAME=${SMOKE_SNAP_NAME:-u-studio-video-editor}
if [ "$RUNNER" = flatpak ]; then
    RUN_HOME=${SMOKE_HOME:-$REAL_HOME/.cache/ustudio-smoke-home}
    MEDIA=$RUN_HOME/media
else
    RUN_HOME=$REAL_HOME
    MEDIA=$REAL_HOME/ustudio-smoke-media
fi
mkdir -p "$RUN_HOME" "$MEDIA"

# env -i: the caller's environment may point XDG_DATA_HOME and GTK/GIO
# module paths elsewhere (a VS Code snap terminal does), which would move
# `flatpak --user` to another installation and break GTK in the test.
env -i HOME="$RUN_HOME" USER="$USER" PATH=/usr/local/bin:/usr/bin:/bin:/snap/bin LANG=C.UTF-8 \
    XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" \
    FLATPAK_USER_DIR="$REAL_HOME/.local/share/flatpak" \
    XDG_DATA_DIRS="$REAL_HOME/.local/share/flatpak/exports/share:/var/lib/flatpak/exports/share:/usr/local/share:/usr/share" \
    SMOKE_RUNNER="$RUNNER" SMOKE_OUT="$OUT" SMOKE_MEDIA="$MEDIA" SMOKE_REAL_HOME="$REAL_HOME" \
    SMOKE_APP_ID="$APP_ID" SMOKE_SNAP_NAME="$SNAP_NAME" SMOKE_HERE="$HERE" GTK_A11Y=atspi \
    dbus-run-session -- sh "$HERE/steps.sh" 2>&1 | tee "$OUT/run.log"
FAILED=$(tail -1 "$OUT/run.log" | sed -n 's/^RESULT: \([0-9]*\) failed.*/\1/p')

if [ "$CLEANUP" = "--cleanup" ]; then
    if [ "$RUNNER" = flatpak ]; then
        rm -rf "$RUN_HOME"
        echo "cleanup: removed $RUN_HOME (test media, projects and the app's test state)"
    else
        rm -rf "$MEDIA" "$REAL_HOME/snap/$SNAP_NAME"
        echo "cleanup: removed $MEDIA and $REAL_HOME/snap/$SNAP_NAME"
    fi
fi
[ "$FAILED" = 0 ]
