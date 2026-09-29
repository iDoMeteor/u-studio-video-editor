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
# The whole screen, as the editor opens maximised on a desktop: wide
# enough for the inspector to dock beside the picture.
python3 "$SMOKE_DRIVE/fitwin.py" >/dev/null 2>&1
sleep 2

# A clip on the timeline, selected; the Rack open.
d act import; sleep 1.5; d loc "$M/clip.mp4"; d enter; sleep 4
d act select-all; sleep 1
d press "Inspector"; sleep 1
d press "Effects" exact; sleep 1
shot 1-rack-empty
check "rack shows the clip" grep -q "\[effects\] health scan" "$OUT/app.log"

# The Browser (E): tiles of the frame through each effect; searching
# "glow" and selecting a tile auditions it on the preview; Enter adds the
# best match.
e() { python3 "$SMOKE_HERE/entry.py" "$@" 2>>"$OUT/helpers.err"; }
d act effects-browser; sleep 1
shot 2a-browser-featured
e "Search effects" glow; sleep 4
shot 2-browser
# Featured effects are checked first; Glow is one. Wait for its result.
for _ in $(seq 1 60); do grep -q '"frei0r.glow"' "$OUT/home/cache/ustudio/effect-health.json" 2>/dev/null && break; sleep 1; done
sleep 1
REBUILDS=$(grep -c "rebuildAll took" "$OUT/app.log")
python3 "$SMOKE_HERE/tile.py" 0 2>>"$OUT/helpers.err"; sleep 3
shot 2b-audition
check "audition didn't rebuild the live graph" [ "$(grep -c "rebuildAll took" "$OUT/app.log")" = "$REBUILDS" ]
check "audition rendered off the live graph" grep -q "auditioning frei0r.glow on the preview" "$OUT/app.log"
shot 2c-looks-ranked-after
d press "Search effects"; sleep 0.5; d enter; sleep 2
d press "Effects" exact; sleep 1
shot 3-glow-added
d act save; sleep 1.5; d settext "$OUT/smoke.ustudio"; d enter; sleep 2
check "saved with the effect" saved_has "frei0r.glow"
d act undo; sleep 1
d act save; sleep 2
check "undo removes it" saved_lacks "frei0r.glow"
shot 4-undone

# Keyframes: redo the effect, set Blur, pin it at frame 0 (P), move 60
# frames on and change it (a second key, added by changing the value).
v() { python3 "$SMOKE_HERE/value.py" "$@" 2>>"$OUT/helpers.err"; }
d act redo; sleep 1.5
d act seek-home; sleep 0.5
v 0 0.5; sleep 1
d act effects-pin; sleep 1.5
for _ in 1 2 3 4 5 6; do d act step-forward-10 >/dev/null; done; sleep 1
v 0 1.0; sleep 1.5
shot 5-keyframed
d act save; sleep 2
check "saved animated: two keys" grep -q '<property name="0">0=0.5;60=1</property>' "$OUT/smoke.ustudio"
d act seek-home; sleep 1
shot 6-back-at-start

# Save the stack as a look (it's in the project), then a second clip: both
# selected, a brand look from the Browser goes on both, and a value
# changed in the Rack changes on both.
d act effects-save-look; sleep 1
e "Look name" "Smoke Look"; sleep 0.5
d enter; sleep 1
d act save; sleep 2
check "look saved in the project" grep -q "Smoke Look" "$OUT/smoke.ustudio"
d act import; sleep 1.5; d loc "$M/clip.mp4"; d enter; sleep 4
d act select-all; sleep 1
d act effects-browser; sleep 1
e "Search effects" "neon night"; sleep 2
d press "Search effects"; sleep 0.3; d enter; sleep 2
d press "Effects" exact; sleep 1.5
shot 7-two-clips-look
d act save; sleep 2
check "brand look on both clips" [ "$(grep -c '<property name="mlt_service">frei0r.softglow</property>' "$OUT/smoke.ustudio")" -ge 2 ]
v 0 0.123; sleep 1.5
d act save; sleep 2
shot 8-two-clips-edited
check "one change, both clips" [ "$(grep -c '>0.123<' "$OUT/smoke.ustudio")" -ge 2 ]

# One drag: a Browser tile onto the picture adds it to the clip under the
# playhead. The window sits at the screen's top left on Xvfb (no window
# manager), so these are its coordinates: the first tile, then the
# picture's middle.
d act effects-browser; sleep 1
e "Search effects" "film grain"; sleep 3
shot 9-before-drag
# The second "Film Grain" tile is the brand Look (an effect ranks first).
TILE=$(python3 "$SMOKE_HERE/where.py" "Film Grain" 1 2>>"$OUT/helpers.err")
PICTURE=$(python3 "$SMOKE_HERE/where.py" "Effects drop zone" 2>>"$OUT/helpers.err" || echo "600 300")
echo "tile at $TILE, picture at $PICTURE" >>"$OUT/steps.log"
# shellcheck disable=SC2086
python3 "$SMOKE_HERE/drag.py" $TILE $PICTURE 2>>"$OUT/helpers.err"; sleep 2
shot 10-after-drag
d act save; sleep 2
check "a brand look dragged onto the picture" grep -q '<property name="mlt_service">frei0r.filmgrain</property>' "$OUT/smoke.ustudio"

# Compare: before (no clip effects) left of the divider, after right; and
# \ held shows the whole picture without them.
d act select-all; sleep 0.5
d act effects-compare; sleep 3
shot 11-compare
check "compare shows the before frame" grep -q "comparing: the before frame shown" "$OUT/app.log"
d act effects-compare; sleep 1
# Focus out of the Browser's search first: in a text field \ is typing.
d click 900 900; sleep 1
python3 "$SMOKE_HERE/hold.py" 51 3 "$OUT/12-held-backslash.png" 2>>"$OUT/helpers.err"
check "held \\ shows the picture without effects" grep -q "held \\\\: the picture without effects" "$OUT/app.log"


# Transitions (FX3): two stills at the end of the track (a still has frames
# to spare either side, so a dissolve fits), T at the cut between them, a
# wipe's tile on the Transitions page, saved with its map beside the
# project; undo takes the style back to the dissolve.
ffmpeg -loglevel error -y -f lavfi -i "color=c=0x2040c0:size=1280x720" -frames:v 1 "$M/still.png"
d act seek-end; sleep 0.5
d act import; sleep 1.5; d loc "$M/still.png"; d enter; sleep 3
d act seek-end; sleep 0.5
d act import; sleep 1.5; d loc "$M/still.png"; d enter; sleep 3
d act seek-end; sleep 1
d act effects-add-transition; sleep 2
shot 13-transition-added
d act save; sleep 2
check "T adds a transition" grep -q "ustudio:transition_id" "$OUT/smoke.ustudio"
# The first wipe's tile (the list scrolls; later tiles may be out of view).
WIPE=$(python3 "$SMOKE_HERE/where.py" "Wipe Right" 2>>"$OUT/helpers.err")
echo "wipe tile at $WIPE" >>"$OUT/steps.log"
# shellcheck disable=SC2086
[ -n "$WIPE" ] && d click $WIPE; sleep 2
shot 14-wipe
d act save; sleep 2
check "a wipe chosen from its tile" grep -q ">wipe.left<" "$OUT/smoke.ustudio"
check "its map written beside the project" [ -s "$OUT/ustudio-wipes/v1/left.pgm" ]
check "the saved file names the map relatively" grep -q ">ustudio-wipes/v1/left.pgm<" "$OUT/smoke.ustudio"
d act undo; sleep 1
d act save; sleep 2
check "undo takes the wipe back" saved_lacks ">wipe.left<"

kill -TERM $APP 2>/dev/null; sleep 2; kill -KILL $APP 2>/dev/null
kill $REGISTRY $LAUNCHER $XVFB 2>/dev/null
echo "RESULT: $FAILED failed"
