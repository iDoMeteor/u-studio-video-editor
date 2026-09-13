# u Studio Video Editor

A from-scratch, GNOME-native video editor for Linux: GTK4 + libadwaita for
the UI, [MLT](https://www.mltframework.org/) for playback/rendering. No Qt,
no KDE Frameworks anywhere in the stack.

This is **not** a port of kdenlive. Kdenlive is ~200,000 lines of C++ built
on 20 KDE Frameworks modules plus Qt Widgets/QML — there's no mechanical way
to translate that to GTK4. This project reuses the same underlying engine
(MLT is a plain C/C++ library with zero KDE/Qt dependency baked into its own
API) behind an entirely new UI, grown incrementally from an initial
single-track skeleton.

## Current capabilities

- Import media files onto any track (`GtkFileDialog`).
- Multi-track timeline: add/remove tracks, drag a track's handle to reorder
  it, click a row to make it the active track (where imports/splits land).
  Higher tracks composite over lower ones for video (full-frame, top wins);
  all tracks mix together for audio, including tracks that are audio-only.
- Play/pause, scrub (including mid-playback), split a clip at the playhead.
- Drag a clip's body to move it — within a track or to a different one.
  Drag near a clip's left/right edge (~8px) to trim it shorter or longer.
  Right-click a track for a "Remove Track" popover.
- Save/load a project (its own format — see "Project files" below).
- Timestamped debug/info/warn/error logging to `logs/`, level configurable
  via `USTUDIO_LOG_LEVEL` (`debug`/`info`/`warn`/`error`/`none`, default
  `info`).

Not yet: effects, titling, undo/redo, waveform display, proxy/transcode,
export/render UI (MLT can render via an `avformat` consumer — just not
wired to the UI yet).

## Building

One system package is required beyond what's typically already on a GNOME
dev machine:

```sh
sudo dnf install mlt-devel
```

PulseAudio's simple API is also needed for audio output (works against this
machine's PipeWire-Pulse compatibility layer):

```sh
sudo dnf install pulseaudio-libs-devel
```

(GTK4, libadwaita, meson, and ninja are assumed already present.)

```sh
meson setup builddir
meson compile -C builddir
./builddir/src/u-studio-video-editor
```

Confirm there's no Qt/KDE anywhere in the link:

```sh
ldd builddir/src/u-studio-video-editor | grep -iE 'qt|kde'   # expect no output
```

For verbose logging during development:

```sh
USTUDIO_LOG_LEVEL=debug ./builddir/src/u-studio-video-editor
```

## Architecture

- `src/engine/mlt_engine.{h,cpp}` — the *only* module that includes an MLT
  header. Owns a `Mlt::Profile`, one `Mlt::Playlist` per track wired into a
  `Mlt::Tractor`, and a dedicated worker thread that pulls one decoded frame
  (image + audio) at a time and hands the image to the GTK main thread via
  `g_idle_add()`. Everything above this module deals only in plain C++ types
  — no MLT, no GTK inside the engine.
- `src/ui/app_window.{h,cpp}` — the app shell (header bar, preview pane,
  multi-row timeline, transport bar), built imperatively against GTK4's C
  API (no `.ui`/GResource files — fewer moving parts).
- `src/ui/style_css.h` — a GTK4 CSS theme mapping the
  [Unicorn Tears design system](~/projects/unicorn-tears/claude-design-system)'s
  color tokens onto libadwaita's named colors (`@accent_bg_color`,
  `@window_bg_color`, etc.), so the whole shell reskins without per-widget
  overrides.
- `src/util/log.{h,cpp}` — the logging module described above.

### Playback engine notes

- **Pull-based, not push.** Rather than a push-style custom `Mlt::Consumer`,
  `MltEngine` pulls frames on its own thread and drives playback itself.
- **Never re-seek every sequential frame.** Calling `seek(position + 1)`
  before every frame during normal forward playback forces MLT's avformat
  producer to treat each step as a random-access seek instead of a cheap
  sequential decode — on a real file with long GOPs/B-frames this measurably
  desyncs audio (confirmed: it cut a real 88s clip's actual audio content
  down to the first ~43s). `get_frame()` already auto-advances the
  producer's position; only call `seek()` for an actual jump/scrub.
- **Pace playback off a wall-clock schedule, not the audio write.** A
  blocking `pa_simple_write()` looks like a natural pacing signal, but isn't
  a safe one: the instant the server's buffer underruns, the next write
  returns instantly instead of blocking, and the loop races ahead — audible
  as alternating fast bursts and static. The schedule anchor must be reset
  on *every* seek that happens during playback (not just on
  paused→playing), or a mid-playback scrub leaves it computing drift against
  a stale reference point forever, which stops all pacing and can peg a
  core badly enough that the desktop force-kills the window as
  unresponsive.
- **`playlist.get_clip()` returns a "cut" producer.** Its own `resource`
  property is a placeholder string (`"<producer>"`); the real file path is
  on `clip->parent()`. Matters for anything that needs the original file
  path (save, clip labels) — confirmed empirically with a standalone repro.
- **Multi-track audio does not mix by default.** An explicit `"mix"`
  transition is required between tracks, and it needs `start=1` (constant
  full level, not a crossfade) *and* `sum=1` (the default halve-then-add
  algorithm measured no different from not mixing at all in testing).
  `"composite"` (video) needed no such tuning — full-frame top-track-wins is
  its default with no geometry configured.

### Clip move/trim implementation notes

Three MLT playlist facts, each verified empirically before relying on them:

- **`split_at()` works on blanks, not just real clips** — splitting a blank
  region at an exact frame gives two smaller blanks. Combined with
  `get_clip_index_at()` + `remove()` + `insert(producer, index, in, out)`,
  this carves an exact-length hole anywhere in a playlist (blank or real
  content) and drops a clip into it at a precise frame — the basis for both
  `moveClip()` and `trimClipStart()`.
- **`resize_clip()` only ever moves a clip's *end*.** Changing `out` moves
  the end directly (the common case, used by `trimClipEnd()` — later clips
  ripple to fill/vacate space, which is the intended ripple-trim). Changing
  `in` instead does *not* move the timeline start — it still shrinks/grows
  from the end, just while showing a different slice of the source. There
  is no `resize_clip` call that keeps a clip's end fixed while moving its
  start. `trimClipStart()` instead picks the clip up
  (`replace_with_blank`) and re-carves it in at the new start with an
  adjusted `in`, reusing the same carve-and-insert primitive as `moveClip`.
- **This app only supports dropping into empty (blank) space** — `moveClip`
  and `trimClipStart` both validate the whole destination span is blank (or
  past the track's current end) before touching anything, and refuse
  otherwise. No overwrite, no ripple-insert-and-shift-everything-after.
  Simpler and much harder to accidentally destroy content with; a real
  "insert and ripple the rest of the track forward" edit mode is a
  reasonable future addition, not attempted here.

### Project files

Save/load uses a small `GKeyFile`-format file (`.ustudio`), **not** MLT's
own XML format, despite MLT natively supporting XML project serialization
via its `xml` consumer/producer. Saving that way works fine, but reloading
does not give back an editable structure: the reloaded XML producer reports
`Service::type() == mlt_service_producer_type`, not
`mlt_service_tractor_type` — wrapping it as `Mlt::Tractor` silently yields
zero tracks and segfaults on playback (confirmed empirically). Rather than
depend on undocumented internals to unwrap it, the project format is just
enough to reconstruct the timeline via the same `addTrack()`/`append()`
calls used for normal editing: track count, and per clip its source file
path plus in/out trim points (so split cuts round-trip correctly).
