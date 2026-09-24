#!/bin/bash
# Runs one driver script against the app under Xvfb (X11, so AT-SPI's
# synthetic mouse and keyboard events reach GTK), inside dbus-run-session,
# with scratch XDG directories and a dummy SDL audio device.
#
#   run.sh <driver.py> <outdir-name> [asan|plain]
#
# SESS: a working directory holding session.sh (from
# ../2026-09-23-sanitizer-run/), the driver scripts, media/ and seed/ (the
# generated project as an untitled autosave, see README.md).
# REPO: the checkout whose builddir/ and builddir-asan/ to run.
# RENDER_WAIT and GDK_DEBUG_X pass through to render_quit.py and GTK.
set -u
SESS=${SESS:?set SESS to the session working directory}
REPO=${REPO:?set REPO to the repository checkout}
OUT=$SESS/$2
rm -rf "$OUT" && mkdir -p "$OUT"
rm -rf "$SESS/state" && mkdir -p "$SESS/state/ustudio/autosave" && cp "$SESS"/seed/* "$SESS/state/ustudio/autosave/"
BIN=$REPO/builddir-asan/src/app/u-studio-video-editor
[ "${3:-asan}" = plain ] && BIN=$REPO/builddir/src/app/u-studio-video-editor
cd "$SESS" && timeout 600 env -i RENDER_WAIT="${RENDER_WAIT:-}" HOME="$HOME" PATH=/usr/bin:/bin USER="$USER" \
    XDG_STATE_HOME="$SESS/state" XDG_DATA_HOME="$SESS/data" XDG_CACHE_HOME="$SESS/cache" \
    XDG_CONFIG_HOME="$SESS/config" XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
    GDK_BACKEND=x11 GDK_DEBUG="${GDK_DEBUG_X:-}" SDL_AUDIODRIVER=dummy USTUDIO_LOG_LEVEL=debug \
    GSETTINGS_SCHEMA_DIR="$REPO/builddir/data" \
    ASAN_OPTIONS="detect_leaks=1:log_path=$OUT/asan" \
    LSAN_OPTIONS="suppressions=$REPO/tests/sanitizers/lsan.supp" \
    UBSAN_OPTIONS="print_stacktrace=1:log_path=$OUT/ubsan" \
    xvfb-run -a -s "-screen 0 1920x1080x24" dbus-run-session -- bash ./session.sh "./$1" "$BIN" "$OUT" x \
    >/dev/null 2>&1
echo "rc $?"
