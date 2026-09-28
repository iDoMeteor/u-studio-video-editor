#!/bin/bash
# One-command demo recording for the rolling series.
#   make_demo.sh    run from the demo worktree (branch agent/strategist-demo): merges the
#                   latest origin/main into it, builds its own release builddir, records.
# Stages media from the owner's drive (read-only) into ~/.cache/ustudio-demo-media once,
# records the tour, and saves the result into the demo-videos folder without ever
# overwriting or removing an existing video.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEST=/home/jj/projects/u-studio-video-editor-projects/demo-videos
MEDIA=${TOUR_MEDIA:-$HOME/.cache/ustudio-demo-media}
AI=/run/media/jj/Expansion/Work/video/exports/ai-generated
WORK=${TMPDIR:-/tmp}/ustudio-demo-run-$$

# 1. media (copies; the originals are only read)
if [ ! -f "$MEDIA/.complete" ]; then
  [ -d "$AI" ] || { echo "drive not mounted: $AI"; exit 1; }
  U=$AI/a-semi-abstract-fantasy-story-about-a-lonely-unicorn-dj-wand
  mkdir -p "$MEDIA"/{unicorn,zizzle,stills,finals}
  for i in 0 1 2 3 4 5 6 7 8 9; do cp -n "$U/video/chunk_00$i.mp4" "$MEDIA/unicorn/"; done
  cp -n "$U/video/a-semi-abstract-fantasy-story-about-a-lonely-unicorn-dj-wand-title-card.mp4" "$MEDIA/unicorn/title-card.mp4"
  for i in 0 1 2 3 4 5 6 7; do cp -n "$AI/zizzle-zap-zone/video/chunk_00$i.mp4" "$MEDIA/zizzle/"; done
  cp -n "$U/thumbnail.png" "$MEDIA/stills/unicorn-thumbnail.png"
  cp -n "$AI/cog-of-the-quark/thumbnail.png" "$MEDIA/stills/cog-thumbnail.png"
  cp -n "$AI/zizzle-zap-zone/images/chunk_000.png" "$MEDIA/stills/zizzle-000.png"
  cp -n "$U/a-semi-abstract-fantasy-story-about-a-lonely-unicorn-dj-wand-final.mp4" "$MEDIA/finals/unicorn-dj-final.mp4"
  touch "$MEDIA/.complete"
fi

# 2. current main, built as release in this worktree (never in the owner's checkout)
# TOUR_NO_MERGE=1 records the build as it is (the one the dry run passed on).
if [ "${TOUR_NO_MERGE:-0}" != 1 ]; then
  git -C "$REPO" fetch -q origin && git -C "$REPO" merge -q --no-edit origin/main
fi
BUILD=$REPO/builddir
[ -d "$BUILD" ] || meson setup "$BUILD" "$REPO" >/dev/null
meson configure "$BUILD" -Dbuildtype=release -Ddropin_titles=builtin >/dev/null   # the titles chapters need the designer
meson compile -C "$BUILD" >/dev/null
VERSION=$(meson introspect "$BUILD" --projectinfo | python3 -c 'import json,sys; print(json.load(sys.stdin)["version"])')

# 3. record + post
TOUR_MEDIA="$MEDIA" TOUR_BIN="$BUILD/src/app/u-studio-video-editor" "$HERE/run_tour.sh" "$WORK" 1
TOUR_VERSION="$VERSION" python3 "$HERE/post.py" "$WORK" "$WORK/demo.mp4"

# 4. save, never overwrite
mkdir -p "$DEST"
BASE="$DEST/u-studio-demo-$(date +%F)-v$VERSION"
OUT="$BASE.mp4"; n=2
while [ -e "$OUT" ]; do OUT="$BASE-$n.mp4"; n=$((n + 1)); done
cp "$WORK/demo.mp4" "$OUT"
echo "saved $OUT"
echo "run folder (screenshots, logs): $WORK"
