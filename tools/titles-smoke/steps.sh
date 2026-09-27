#!/bin/sh
# run.sh's steps, inside the private D-Bus session.
set -u
Xvfb "$DISPLAY" -screen 0 1600x1000x24 -nolisten tcp >"$SMOKE_OUT/xvfb.log" 2>&1 &
XVFB=$!
sleep 1
/usr/libexec/at-spi-bus-launcher --launch-immediately >/dev/null 2>&1 &
LAUNCHER=$!
for _ in $(seq 1 40); do
    gdbus call --session --dest org.a11y.Bus --object-path /org/a11y/bus --method org.a11y.Bus.GetAddress \
        >/dev/null 2>&1 && break
    sleep 0.25
done
/usr/libexec/at-spi2-registryd >/dev/null 2>&1 &
REGISTRY=$!
sleep 1
USTUDIO_LOG_LEVEL=debug GTK_A11Y=atspi "$SMOKE_APP" >"$SMOKE_OUT/app.log" 2>&1 &
APP=$!
python3 "$SMOKE_HERE/drive.py" lower-third
kill -TERM $APP 2>/dev/null; sleep 1; kill -KILL $APP 2>/dev/null
kill $REGISTRY $LAUNCHER $XVFB 2>/dev/null
