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
  Right-click a clip to delete it (leaves a gap — "lift", nothing else
  moves); right-click a gap to close it (ripples later content earlier to
  fill it); right-click empty track space for "Remove Track".
- Per-clip audio waveforms, drawn on every clip that has audio (video or
  audio-only), computed on a background thread so the UI never stalls —
  see `src/engine/waveform_cache.{h,cpp}`.
- Render the project to an MP4 matching this project's fixed working
  format (H.264 High/yuv420p, 1920×1080, 30fps, AAC 48kHz stereo) via the
  header bar's "Render…" button. Runs on a background thread.
- Save/load a project (its own format — see "Project files" below).
- Timestamped debug/info/warn/error logging to
  `$XDG_STATE_HOME/ustudio/logs/` (falls back to
  `~/.local/state/ustudio/logs/` if `XDG_STATE_HOME` is unset), level
  configurable via `USTUDIO_LOG_LEVEL` (`debug`/`info`/`warn`/`error`/`none`,
  default `info`).
- No Qt/KDE anywhere in the *running process*, not just the link line —
  `FactoryPolicy` curates the MLT module directory at startup so Qt6's MLT
  modules never get `dlopen`'d. See "Architecture" below.

Not yet: effects, titling, undo/redo, proxy/transcode, configurable export
formats (render is currently hardcoded to this project's own working
format — see "Render implementation notes" below), ripple/overwrite editing
beyond move/trim's "destination must be empty" rule.

## Roadmap

Active development is following a v2 rewrite plan recorded under
[`docs/plans/v2/`](docs/plans/v2/) — architecture, project model, milestones
(M0–M7), and the ADRs that are the binding contract for load-bearing
decisions. Milestone M0 (this restructure: `core/engine/app/render` layout,
tests, CI, packaging metadata — no behavior change) is the current one in
flight; see [`docs/plans/v2/12-roadmap-and-milestones.md`](docs/plans/v2/12-roadmap-and-milestones.md)
for what ships in what order. `CLAUDE.md` governs day-to-day coding/agent
conventions for this repo.

## Building

System packages needed beyond what's typically already on a GNOME dev
machine:

```sh
sudo dnf install mlt-devel pulseaudio-libs-devel
```

(GTK4, libadwaita, GLib/GIO, libxml2, meson, and ninja are assumed already
present — libxml2 in particular is normally already pulled in as an MLT
transitive dependency.) doctest (M0's test framework) needs no system
package — it's vendored under `subprojects/doctest/`.

### Development tools

Two more tools are needed to actually run the `.clang-format`/`justfile`
deliverables locally (not just have the files present):

```sh
sudo dnf install clang-tools-extra just
```

- **`clang-tools-extra`** provides `clang-format`, the formatter `.clang-format`
  configures and `just fmt` / CI's `format` job run.
- **`just`** is the command-runner behind the `justfile` recipes below (a
  thin wrapper so local and CI invocations stay identical — see
  [`docs/plans/v2/11-build-test-ci-packaging.md`](docs/plans/v2/11-build-test-ci-packaging.md)).
  Without it, run the underlying `meson`/`ninja`/`clang-format` commands
  directly (each recipe's `justfile` body shows the equivalent command).

### Build, run, test

With `just` installed:

```sh
just setup   # meson setup builddir -Dbuildtype=debug -Dtests=enabled
just build   # meson compile -C builddir
just test    # meson test -C builddir --print-errorlogs
just run     # ./builddir/src/app/u-studio-video-editor
just fmt     # clang-format -i on every tracked .cpp/.h
```

Without `just`, the equivalent plain commands:

```sh
meson setup builddir
meson compile -C builddir
meson test -C builddir --print-errorlogs
./builddir/src/app/u-studio-video-editor
```

Confirm there's no Qt/KDE anywhere in the link *or the running process*:

```sh
ldd builddir/src/app/u-studio-video-editor | grep -iE 'qt|kde'   # expect no output (link line)
./builddir/tests/engine/test_factory_policy                      # expect PASS (runtime: /proc/self/maps has no libQt)
```

For verbose logging during development:

```sh
USTUDIO_LOG_LEVEL=debug ./builddir/src/app/u-studio-video-editor
```

## Architecture

Layered as `core` (pure C++23, no GTK/MLT) → `engine` (the only layer that
touches `mlt++`) → `app` (GTK4/libadwaita; never includes an MLT or Pulse
header directly — enforced by a build-time grep in
`src/app/meson.build`, not just review) and `render` (a headless CLI
skeleton for now; the real implementation is milestone M6). Full rationale
in [`docs/plans/v2/02-architecture.md`](docs/plans/v2/02-architecture.md).

- `src/core/log.{h,cpp}` (`ustudio::core::Log`) — the logging module, thread-
  safe, no dependencies. Writes to `$XDG_STATE_HOME/ustudio/logs/`.
- `src/engine/factory_policy.{h,cpp}` (`ustudio::engine::FactoryPolicy`) —
  owns `Mlt::Factory::init()`/`close()` for the whole process: exactly one
  instance, constructed in `main()` before any window or `MltEngine`,
  destroyed after `g_application_run()` returns. Builds a curated MLT
  module directory under `$XDG_RUNTIME_DIR/ustudio-mlt-modules/` (or, when
  `XDG_RUNTIME_DIR` isn't set — CI containers and other headless
  environments don't have a logind session — under the system temp
  directory instead) containing symlinks to every module except a
  `qt6`/`glaxnimate-qt6` denylist, so
  `Mlt::Factory::init()` never `dlopen`s Qt6 — a plain, argument-less
  `Factory::init()` measurably does (32 Qt library mappings observed on
  this machine); the curated approach was verified, via a standalone
  repro, to give zero Qt mappings while every consumer/transition this app
  needs stays available. See ADR-007. `MltEngine` itself no longer calls
  `Factory::init()`/`close()` — doing so from two places would silently
  re-run init with the wrong (default) directory the second time.
- `src/engine/mlt_engine.{h,cpp}` (`ustudio::engine::MltEngine`) — the main
  module touching MLT types for live editing/playback. Owns a
  `Mlt::Profile` (fixed at `atsc_1080p_30` — 1920×1080/30fps — matching
  this project's real source/export format; see "Render implementation
  notes"), one `Mlt::Playlist` per track wired into a `Mlt::Tractor`, and a
  dedicated worker thread that pulls one decoded frame (image + audio) at
  a time and hands the image to the GTK main thread via `g_idle_add()`.
  Everything above this module deals only in plain C++ types — no MLT, no
  GTK inside the engine.
- `src/engine/waveform_cache.{h,cpp}` (`ustudio::engine::WaveformCache`) —
  a small, separate MLT touchpoint (opens its own throwaway
  `Profile`/`Producer` per clip) for background audio-peak extraction,
  kept outside `MltEngine` specifically so it never contends with
  `m_mltMutex` or blocks editing/playback.
- `src/app/main.cpp`, `src/app/app_window.{h,cpp}`
  (`ustudio::app::AppWindow`) — the app shell (header bar, preview pane,
  multi-row timeline, transport bar), built imperatively against GTK4's C
  API (no `.ui` files for the shell — fewer moving parts).
- `src/app/style/style.css` — a GTK4 CSS theme mapping the
  [Unicorn Tears design system](~/projects/unicorn-tears/claude-design-system)'s
  color tokens onto libadwaita's named colors (`@accent_bg_color`,
  `@window_bg_color`, etc.), so the whole shell reskins without per-widget
  overrides. Compiled into the binary via GResource
  (`data/ustudio.gresource.xml`), loaded at startup with
  `gtk_css_provider_load_from_resource()`.
- `src/render/main.cpp` — placeholder; the real headless render CLI
  (`u-studio-render`) is milestone M6.
- `tests/core/`, `tests/engine/` — doctest suites (vendored under
  `subprojects/doctest/`, no system package needed). `tests/engine/`
  needs MLT but no display and no media files.

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

### Render implementation notes

`renderProject()` uses MLT's `avformat` consumer, with properties confirmed
against its actual YAML metadata rather than guessed from ffmpeg CLI-flag
muscle memory (`vcodec`/`acodec`/`f`/`vb`/`ab`/`ar`/`channels`/`pix_fmt`/
`real_time` — not, say, `b:v`/`crf`). The target format itself — h264 High
profile, yuv420p, 1920×1080, 30fps, ~923kbps video / AAC-LC 48kHz stereo
~126kbps, MP4 — was read directly off a real reference export file via
`ffprobe`, and the whole pipeline (profile choice, consumer properties) was
validated with a standalone render-then-reprobe round-trip that confirmed
an exact match before any of it was wired into the engine.

It renders from a completely separate, throwaway `Profile`/`Tractor` built
from a quick snapshot of the current tracks/clips (same
resource/in/out capture as `saveProject()`) rather than through the live
`m_tractor` — so a render, which can take real encode time for a long
project, never holds `m_mltMutex` and never blocks editing or playback
while it runs.

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
