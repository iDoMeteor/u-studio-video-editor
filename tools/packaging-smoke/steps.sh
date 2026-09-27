#!/bin/sh
# The smoke-test steps; run.sh runs this inside a private D-Bus session.
# Every check prints PASS or FAIL; the last line is "RESULT: <n> failed".
set -u
H=$SMOKE_HERE; OUT=$SMOKE_OUT; M=$SMOKE_MEDIA
FAILED=0
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; FAILED=$((FAILED + 1)); }
check() { name=$1; shift; if "$@"; then pass "$name"; else fail "$name"; fi; }
d() { python3 "$H/drive.py" "$@" 2>/dev/null; }
status() { d status; }
# Only this run's processes: other copies of the editor may be running on the
# machine (the owner's, other agents'). drive.py tags ours via the environment.
ours() {
    for p in $(pgrep -x "$1"); do
        tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -qx "USTUDIO_SMOKE_RUN=$OUT" && echo "$p"
    done | head -1
}
editor_pid() { ours u-studio-video-; }
no_qt() { p=$1; [ -n "$p" ] && [ "$(grep -ciE 'libqt|qt6' "/proc/$p/maps")" = 0 ]; }
last_status_is() { case "$(status)" in "$1"*) true ;; *) echo "  status: $(status)"; false ;; esac; }
import_file() { d act import; sleep 1.5; d loc "$1"; d enter; sleep 3; }

# --- private X display and accessibility bus
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
echo "display $DISPLAY, runner $SMOKE_RUNNER"

# --- test media: generated, never real footage (CLAUDE.md)
mkdir -p "$M"
ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1920x1080:rate=30:duration=10 \
    -f lavfi -i sine=frequency=440:duration=10:sample_rate=48000 \
    -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "$M/clip1080.mp4"
ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=3840x2160:rate=30:duration=6 \
    -f lavfi -i sine=frequency=660:duration=6:sample_rate=48000 \
    -c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac -shortest "$M/clip4k.mp4"
ffmpeg -loglevel error -y -f lavfi -i "color=c=magenta:size=640x360" -frames:v 1 "$M/logo.png"
ffmpeg -loglevel error -y -f lavfi -i "testsrc2=size=1280x720" -frames:v 1 -q:v 3 "$M/still.jpg"
AUDIO="$M/sdl-audio.raw"
LAUNCH="GDK_DEBUG=no-portals SDL_AUDIODRIVER=disk SDL_AUDIO_DRIVER=disk SDL_AUDIO_DISK_OUTPUT_FILE=$AUDIO SDL_DISKAUDIOFILE=$AUDIO"

# --- 0: the installed package carries nothing from a test run or the build machine
if [ "$SMOKE_RUNNER" = flatpak ]; then
    TREE="$(flatpak info --user --show-location "$SMOKE_APP_ID")/files"
else
    TREE="/snap/$SMOKE_SNAP_NAME/current"
fi
check "installed package is clean ($TREE)" python3 "$H/../check_bundle_clean.py" "$TREE" --home "$SMOKE_REAL_HOME"

# --- 1: launch
d launch $LAUNCH
sleep 2
d act new-project
sleep 1
d shot 01-launch
check "curated MLT module directory in use" grep -q "curated MLT module dir" "$OUT/app.log"
check "no Qt mapped at launch" no_qt "$(editor_pid)"

# --- 2: import video and stills
import_file "$M/clip1080.mp4"; check "import H.264/AAC video" last_status_is "Imported"
d press "Add track"; sleep 1; d act active-track-top; d act seek-home
import_file "$M/logo.png"; check "import PNG still" last_status_is "Imported"
d press "Add track"; sleep 1; d act active-track-top; d act seek-home
import_file "$M/still.jpg"; check "import JPEG still" last_status_is "Imported"
d shot 02-imported

# --- 3: transform the top picture on the preview (MLT plus module's affine)
d click 900 300; sleep 1
d drag 960 380 760 300; sleep 1.5
d act transform-rotate-cw; sleep 2
d shot 03-transform
check "transform applied (affine)" grep -q "clip transforms applied in place" "$OUT/app.log"

# --- 4: playback with sound, through SDL's disk driver
before=$(stat -c %s "$AUDIO" 2>/dev/null || echo 0)
d act seek-home; d act play-pause; sleep 1.5
check "no Qt mapped while playing" no_qt "$(editor_pid)"
sleep 1.5; d act play-pause; sleep 1
tail -c +$((before + 1)) "$AUDIO" > "$OUT/play.raw"
tone=$(python3 "$H/tone.py" "$OUT/play.raw"); echo "  $tone"
check "playback produced the 440 Hz tone" sh -c "echo '$tone' | grep -qE 'audible [1-9].*~4[34][0-9] Hz'"

# --- 5: edit, undo, redo, save
d act active-track-bottom; d act seek-home
for _ in 1 2 3 4 5 6 7 8 9; do d act step-forward-10 >/dev/null; done
d act split-at-playhead; sleep 1
d act undo; sleep 0.5; check "undo split" last_status_is "Undid: Split clip"
d act redo; sleep 0.5; check "redo split" last_status_is "Redid: Split clip"
d act save; sleep 1.5; d settext "$M/smoke.ustudio"; d enter; sleep 2
check "save project" last_status_is "Saved"

# --- 6: close, reopen (the last project reopens by itself)
d close; sleep 4
check "clean exit" test -z "$(editor_pid)"
d launch $LAUNCH; sleep 3
check "last project reopens" last_status_is "Opened"
d shot 06-reopened

# --- 7: render with the default (High quality) profile
d press "Render…"; sleep 2; d press Save exact; sleep 3
check "no Qt mapped while rendering" no_qt "$(editor_pid)"
d waitlog "status: Rendered" 240
check "render finished" last_status_is "Rendered"
render=$(ls "$M"/smoke-high-quality-*.mp4 2>/dev/null | tail -1)
if [ -n "$render" ]; then
    streams=$(ffprobe -v error -show_entries stream=codec_name,width,height -of csv=p=0 "$render" | tr '\n' ' ')
    echo "  render streams: $streams"
    check "render is 1080p H.264 + AAC" sh -c "echo '$streams' | grep -q 'h264,1920,1080' && echo '$streams' | grep -q aac"
    ffmpeg -loglevel error -y -ss 1 -i "$render" -frames:v 1 "$OUT/07-render-1s.png"
else
    fail "render file exists"
fi

# --- 8: proxy for 4K footage, made by u-studio-render
d press "Add track"; sleep 1; d act active-track-top; d act seek-home
import_file "$M/clip4k.mp4"
d press "Create Proxies"; sleep 2
check "no Qt mapped in u-studio-render" no_qt "$(ours u-studio-render)"
d waitlog "status: Proxy ready" 120
check "proxy ready" last_status_is "Proxy ready"
d act save; sleep 1.5

# --- 9: Copy Diagnostics
d act copy-diagnostics; sleep 1
python3 "$H/clipboard.py" | sed -n 1,4p | tee "$OUT/diagnostics.txt" | sed 's/^/  /'
case "$SMOKE_RUNNER" in
    flatpak) check "diagnostics say Flatpak: yes" grep -q "^Flatpak: yes" "$OUT/diagnostics.txt" ;;
    *) check "diagnostics list a log folder" grep -q "^Log folder: " "$OUT/diagnostics.txt" ;;
esac

# --- 10: close
d close; sleep 4
check "clean exit at the end" test -z "$(editor_pid)"

kill "$REGISTRY" "$LAUNCHER" "$XVFB" 2>/dev/null
echo "RESULT: $FAILED failed"
