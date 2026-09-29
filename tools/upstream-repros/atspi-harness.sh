#!/bin/sh
# Runs a GTK program and an AT-SPI probe on a private X server, D-Bus session and
# AT-SPI bus, so nothing appears on (or talks to) the desktop session.
#
#   atspi-harness.sh <probe.py> <program> [args...]
#
# Needs Xvfb, dbus-run-session, at-spi2-core (at-spi-bus-launcher, at-spi2-registryd)
# and python3-gobject with the Atspi typelib. The program's output goes to program.log
# in the current directory.
set -u
PROBE=$(realpath "$1")
shift
PROG=$(realpath "$1")
shift
# Services the private session activates (portals) get their own runtime dir, so a
# document portal can't mount over the desktop's.
RT=$(mktemp -d)
N=90
while [ -e "/tmp/.X11-unix/X$N" ]; do N=$((N + 1)); done
env -i HOME="$HOME" PATH=/usr/bin:/bin LANG=C.UTF-8 \
    XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" PRIV_RT="$RT" \
    DISPLAY=":$N" GDK_BACKEND=x11 GDK_DEBUG=no-portals GIO_USE_VFS=local \
    PROBE="$PROBE" PROG="$PROG" ARGS="$*" LOG="$PWD/program.log" \
    dbus-run-session -- sh -c '
        dbus-update-activation-environment XDG_RUNTIME_DIR="$PRIV_RT" GIO_USE_VFS=local
        Xvfb "$DISPLAY" -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 & X=$!
        sleep 1
        /usr/libexec/at-spi-bus-launcher --launch-immediately >/dev/null 2>&1 & L=$!
        for _ in $(seq 1 40); do
            gdbus call --session --dest org.a11y.Bus --object-path /org/a11y/bus \
                --method org.a11y.Bus.GetAddress >/dev/null 2>&1 && break
            sleep 0.25
        done
        /usr/libexec/at-spi2-registryd >/dev/null 2>&1 & R=$!
        sleep 1
        GTK_A11Y=atspi "$PROG" $ARGS >"$LOG" 2>&1 & P=$!
        python3 "$PROBE" "$(basename "$PROG")"
        kill $P; sleep 0.5; kill $R $L $X 2>/dev/null
    ' 2>/dev/null
rm -rf "$RT" 2>/dev/null
