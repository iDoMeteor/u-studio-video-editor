#!/bin/sh
# run.sh's steps, inside the private D-Bus session. Every check prints PASS
# or FAIL; the last line is "RESULT: <n> failed".
set -u
OUT=$SMOKE_OUT; M=$SMOKE_MEDIA
FAILED=0
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; FAILED=$((FAILED + 1)); }
check() { name=$1; shift; if "$@"; then pass "$name"; else fail "$name"; fi; }
d() { python3 "$SMOKE_DRIVE/drive.py" "$@" 2>/dev/null; }
shot() { import -window root "$OUT/$1.png" 2>/dev/null; }
saved_has() { grep -q "$1" "$OUT/smoke.ustudio"; }
saved_lacks() { ! grep -q "$1" "$OUT/smoke.ustudio"; }

dbus-update-activation-environment XDG_RUNTIME_DIR="$SMOKE_RUNTIME" GIO_USE_VFS=local

exec 3>"$OUT/display"
Xvfb -displayfd 3 -screen 0 1920x1080x24 -nolisten tcp >"$OUT/xvfb.log" 2>&1 &
XVFB=$!
for _ in $(seq 1 40); do [ -s "$OUT/display" ] && break; sleep 0.25; done
export DISPLAY=":$(cat "$OUT/display")"
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

# Test media: generated, never real footage (CLAUDE.md).
mkdir -p "$M"
ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1280x720:rate=30:duration=6 \
    -f lavfi -i sine=frequency=440:duration=6:sample_rate=48000 \
    -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "$M/clip.mp4"

SDL_AUDIODRIVER=dummy USTUDIO_LOG_LEVEL=debug GTK_A11Y=atspi USTUDIO_SMOKE_RUN="$OUT" \
    "$SMOKE_APP" >"$OUT/app.log" 2>&1 &
APP=$!
for _ in $(seq 1 60); do
    python3 -c "import sys; sys.path.insert(0, '$SMOKE_DRIVE'); import drive; sys.exit(0 if drive.app_node() else 1)" \
        2>/dev/null && break
    sleep 0.5
done
sleep 2

# A clip on the timeline, selected; the Rack open.
d act import; sleep 1.5; d loc "$M/clip.mp4"; d enter; sleep 4
d act select-all; sleep 1
d press "Inspector"; sleep 1
d press "Effects" exact; sleep 1
shot 1-rack-empty
check "rack shows the clip" grep -q "\[effects\] health scan" "$OUT/app.log"

# Add Glow through the Add search.
d act effects-browser; sleep 1
d settext glow; sleep 1
shot 2-add-search
d enter; sleep 2
shot 3-glow-added
d act save; sleep 1.5; d settext "$OUT/smoke.ustudio"; d enter; sleep 2
check "saved with the effect" saved_has "frei0r.glow"
d act undo; sleep 1
d act save; sleep 2
check "undo removes it" saved_lacks "frei0r.glow"
shot 4-undone

kill -TERM $APP 2>/dev/null; sleep 2; kill -KILL $APP 2>/dev/null
kill $REGISTRY $LAUNCHER $XVFB 2>/dev/null
echo "RESULT: $FAILED failed"
