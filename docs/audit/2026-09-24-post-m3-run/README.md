# Post-M3 audit run artifacts (2026-09-24)

Scripts and repros used for [the post-M3 audit](../2026-09-24-post-m3-audit.md),
kept so each finding can be re-checked after a fix. Nothing here is built by
meson.

## App sessions

These run the app under Xvfb (X11) instead of the owner's Wayland session,
because AT-SPI's synthetic mouse and keyboard events only reach GTK on X11.
GTK4 reports no widget positions over AT-SPI, so timeline coordinates come
from a screenshot of the default 1100x700 window (`drive_base.shot()`).

| File | Purpose |
|---|---|
| `run.sh` | Runs one driver under Xvfb and `dbus-run-session` with scratch XDG dirs and `SDL_AUDIODRIVER=dummy` (see its header) |
| `drive_base.py` | Shared driver: launch, AT-SPI lookups, window actions over D-Bus, synthetic drags with modifier keys, screenshots |
| `stale_selection.py` | Finding H1: Select All, remove an asset from the media browser, Shift+Delete |
| `render_quit.py` | Finding H2: start a render, close the window while it runs (`RENDER_WAIT=0.3 GDK_DEBUG_X=no-portals`) |
| `gestures.py`, `mods.py` | Coverage run: moves, trims, copy, slip, ripple trim, rubber band, ripple mode, nudges, markers, zoom, split, ripple delete, 30-step undo/redo, edits while playing |
| `nudge_play.py` | Finding M1 in the app: 40 nudges per round while playing (one consumer restart each) |
| `zoom.py` | Finding M3: 24 zoom steps, then counts thumbnail decode jobs in the debug log |

Setup: build `mkmedia` and `mkproject` from `../2026-09-23-sanitizer-run/`,
render `media/clip.mp4`, add any PNG as `media/still.png`, then save the
project as `seed/aaaa.ustudio` next to a `seed/aaaa.meta` of
`{"path":"","timestamp":1790000000,"pid":999999,"pid_start":1}` so the app
offers it for recovery. Run helpers under `env -i` if the shell carries a
snap-contaminated environment.

## Core repros

Each builds against the core library alone:

```sh
g++ -std=c++23 -Isrc <file>.cpp builddir/src/core/libustudio_core.a $(pkg-config --libs libxml-2.0)
```

| File | Finding | Prints on 981b70d |
|---|---|---|
| `ripple_noop.cpp` | M2 | `applied; B at 100 C at 200; transitions=0` (dissolve gone, nothing moved) |
| `ripple_short.cpp` | L1 | `ripple B -> 150 ... REFUSED` |
| `marker_redo.cpp` | L2 | aborts in `UndoStack::redo` |

## Playback controller hang (M1)

```sh
# 16 at a time, 4 rounds; hung runs exit 124
SDL_AUDIODRIVER=dummy timeout 60 ./builddir/tests/engine/test_playback_controller \
    -tc="*repeated setTractor*"
```
