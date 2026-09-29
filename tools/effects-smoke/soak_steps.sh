#!/bin/sh
# The effects drop-in's playback memory soak, run.sh's third argument:
#
#   tools/effects-smoke/run.sh <builddir> <outdir> soak_steps.sh
#
# A 1080p H.264 clip, selected; the Browser shown (its tiles open the clip
# in the drop-in's frame renderer), then another page, while the health
# scan's results keep coming (a fresh profile probes every effect). Then
# 9 s looped on the GPU pipeline when it's on, for 12 minutes. The 0.78.0
# Flatpak's "+40 MB/min" was one step of about 180 MB: the hidden Browser
# re-rendering a tile on a scan result, which opened a 1080p decoder its
# renderer then kept for good. So the checks: nothing opens the clip while
# the Browser is hidden, and the idle renderer closes it.
#
# RSS is sampled every 2 s, and after a 120 s warm-up the slope of its
# floor (each 60 s window's minimum: a frame in flight is 8 MB, so single
# samples jitter by tens of MB) is reported against 1 MB/min, as a WARN
# only: under Xvfb the same fixed build read -6 and +18 MB/min in two
# 15-minute runs, and an effects-free build +10 to +22 over 5 minutes
# (2026-09-29): the core GPU pipeline's swings, for VE GPU's quiet soak.
set -u
CLIP_SIZE=1920x1080
CLIP_SECONDS=12
. "$SMOKE_HERE/start.sh"
WARMUP=120
MEASURE=600

d act import; sleep 1.5; d loc "$M/clip.mp4"; d enter; sleep 5
d act select-all; sleep 1
d press "Inspector"; sleep 1
d act effects-browser; sleep 4
check "the Browser rendered its tiles" grep -q "frame renderer: opened" "$OUT/app.log"
d press "Effects" exact; sleep 1
HIDDEN_AT=$(wc -l <"$OUT/app.log")
grep -q "\[gpu\] GPU pipeline on" "$OUT/app.log" && echo "  GPU pipeline on" || echo "  GPU pipeline off (CPU soak)"

d act seek-home; d act loop-set-in
for _ in $(seq 1 27); do d act step-forward-10 >/dev/null; done
d act loop-set-out; d act seek-home
d act play-pause
: >"$OUT/rss.txt"
start=$(date +%s)
while [ $(($(date +%s) - start)) -le $((WARMUP + MEASURE)) ]; do
    echo "$(($(date +%s) - start)) $(awk '/VmRSS/{print $2}' "/proc/$APP/status" 2>/dev/null)" >>"$OUT/rss.txt"
    sleep 2
done
d act play-pause
soak=$(python3 - "$OUT/rss.txt" "$WARMUP" <<'EOF'
import sys
warmup = float(sys.argv[2])
rows = [tuple(map(float, line.split())) for line in open(sys.argv[1]) if len(line.split()) == 2]
rows = [r for r in rows if r[0] >= warmup]
floors = {}
for t, kb in rows:
    w = int((t - warmup) // 60)
    floors[w] = min(floors.get(w, kb), kb)
pts = sorted((warmup + 60 * w + 30, kb) for w, kb in floors.items())
if len(pts) < 4:
    print("slope n/a (too few samples)"); sys.exit()
mx = sum(p[0] for p in pts) / len(pts)
my = sum(p[1] for p in pts) / len(pts)
slope = sum((p[0] - mx) * (p[1] - my) for p in pts) / sum((p[0] - mx) ** 2 for p in pts)  # kB/s
print(f"floor slope {slope * 60 / 1024:+.2f} MB/min over {rows[-1][0] - rows[0][0]:.0f} s, "
      f"RSS {rows[0][1] / 1024:.0f} -> {rows[-1][1] / 1024:.0f} MB")
EOF
)
echo "  playback soak: $soak"
check "nothing opened the clip while the Browser was hidden" \
    sh -c "! tail -n +$((HIDDEN_AT + 1)) '$OUT/app.log' | grep -q 'frame renderer: opened'"
check "the idle frame renderer closed the clip" grep -q "frame renderer: closed" "$OUT/app.log"
if python3 -c "
import re, sys; m = re.search(r'slope ([+-][\d.]+) MB/min', sys.argv[1]); sys.exit(0 if m and float(m.group(1)) < 1.0 else 1)" "$soak"; then
    echo "PASS RSS floor flat during playback (< 1 MB/min)"
else
    echo "WARN RSS floor rose 1 MB/min or more (report only: see the header)"
fi

kill -TERM $APP 2>/dev/null; sleep 2; kill -KILL $APP 2>/dev/null
kill $REGISTRY $LAUNCHER $XVFB 2>/dev/null
echo "RESULT: $FAILED failed"
