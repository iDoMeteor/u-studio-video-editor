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

- Import media files onto any track (`GtkFileDialog`). Still images (PNG,
  JPEG, ...) are detected automatically (via the opened producer's own MLT
  service, not the file extension) and default to spanning the rest of the
  current project length from the insert point — drop one onto an empty top
  track for an instant full-timeline watermark/logo overlay, no manual
  trim-to-fit needed.
- Multi-track timeline: add/remove tracks, drag a track's handle to reorder
  it, click a row to make it the active track (where imports/splits land).
  Higher tracks composite over lower ones for video (full-frame, top wins);
  all tracks mix together for audio, including tracks that are audio-only.
- **Undo/redo** for every edit (header-bar buttons, `Ctrl+Z`/`Ctrl+Shift+Z`),
  backed by a real command/undo-stack model — see "Architecture" below.
- Playback via an MLT consumer (`sdl2_audio`, falling back to `rtaudio`,
  then `null` — see "Playback engine notes"): play/pause, `J`/`K`/`L`
  shuttle (repeated `J`/`L` ramps speed 1x→2x→4x→8x), frame step
  (`Left`/`Right`), jump to start/end (`Home`/`End`), loop in/out (`I`/`O`
  at the playhead), volume, preview-scale preference (Auto/Full/Half/
  Quarter), scrub by dragging the seek bar (including mid-playback).
- Split a clip at the playhead.
- Drag a clip's body to move it — within a track or to a different one.
  Drag near a clip's left/right edge (~8px) to trim it shorter or longer.
  Right-click a clip to delete it (leaves a gap — "lift", nothing else
  moves); right-click a clip that has audio and isn't already audio-only
  for "Split Audio" (pulls its audio out to a new, independent clip on the
  nearest audio track with room, creating one if none has room — the two
  halves can then be moved/trimmed independently); right-click a gap to
  close it (ripples later content earlier to fill it); right-click empty
  track space for "Remove Track".
- Per-clip audio waveforms, drawn on every clip that has audio (video or
  audio-only), computed on a background thread so the UI never stalls —
  see `src/engine/waveform_cache.{h,cpp}`.
- Render the project to an MP4 matching this project's fixed working
  format (H.264 High/yuv420p, 1920×1080, 30fps, AAC 48kHz stereo) via the
  header bar's "Render…" button. Runs on a background thread.
- Save/load a project as MLT XML with `ustudio:` namespaced properties
  (see "Project files" below), plus autosave and crash recovery.
- Timestamped debug/info/warn/error logging to
  `$XDG_STATE_HOME/ustudio/logs/` (falls back to
  `~/.local/state/ustudio/logs/` if `XDG_STATE_HOME` is unset), level
  configurable via `USTUDIO_LOG_LEVEL` (`debug`/`info`/`warn`/`error`/`none`,
  default `info`).
- No Qt/KDE anywhere in the *running process*, not just the link line —
  `FactoryPolicy` curates the MLT module directory at startup so Qt6's MLT
  modules never get `dlopen`'d. See "Architecture" below.
- No PulseAudio/`libpulse-simple` dependency — audio output goes through
  the same MLT consumer as video.

Not yet: effects, titling, proxy/transcode, configurable export formats
(render is currently hardcoded to this project's own working format — see
"Render implementation notes" below), ripple/overwrite editing beyond
move/trim's "destination must be empty" rule, a real ruler/ripple-timeline
widget (loop in/out and scrubbing use a plain `GtkScale` for now).

## Roadmap

Active development is following a v2 rewrite plan recorded under
[`docs/plans/v2/`](docs/plans/v2/) — architecture, project model, milestones
(M0–M7), and the ADRs that are the binding contract for load-bearing
decisions. M0 (the `core/engine/app/render` restructure) and M1 (project
model, commands, undo/redo, MLT-XML save/load, autosave/recovery) have
landed; M2 (playback via a real MLT consumer instead of a hand-rolled pull
loop, per ADR-002) is in flight — its transport-UI and engine-swap work is
in, but its real-hardware acceptance criteria (4K60 real-time playback,
a 10-minute no-growth soak) haven't been exercised yet. See
[`docs/plans/v2/12-roadmap-and-milestones.md`](docs/plans/v2/12-roadmap-and-milestones.md)
for what ships in what order. `CLAUDE.md` governs day-to-day coding/agent
conventions for this repo.

## Building

System packages needed beyond what's typically already on a GNOME dev
machine:

```sh
sudo dnf install mlt-devel
```

(GTK4, libadwaita, GLib/GIO, libxml2, meson, and ninja are assumed already
present — libxml2 in particular is normally already pulled in as an MLT
transitive dependency.) doctest (M0's test framework) needs no system
package — it's vendored under `subprojects/doctest/`. No PulseAudio
package is needed: playback audio goes through MLT's own `sdl2_audio`/
`rtaudio` consumer (see "Playback engine notes"), and both modules ship
in the base `mlt-devel`/`mlt` install.

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

Layered as `core` (pure C++23, no GTK/MLT — the project model, commands,
undo stack, and XML persistence) → `engine` (the only layer that touches
`mlt++`) → `app` (GTK4/libadwaita; never includes an MLT header directly —
enforced by a build-time grep in `src/app/meson.build`, not just review)
and `render` (a headless CLI skeleton for now; the real implementation is
milestone M6). Full rationale in
[`docs/plans/v2/02-architecture.md`](docs/plans/v2/02-architecture.md).

- `src/core/model/` (`ustudio::core::Model`) — the source of truth for
  everything the user edits (ADR-003): `Project`/`Sequence`/`Track`/`Clip`/
  `Asset` value types, mutators that validate-mutate-emit, and a
  `Model::changed` signal every downstream projection (the MLT engine, the
  UI) subscribes to instead of being told to resync by hand.
- `src/core/commands/` — one `Command` per mutator (`InsertClip`,
  `MoveClip`, `ResizeClip`, `RemoveTrack`, ...), a `CompositeCommand` for
  multi-step edits (e.g. "close gap" = N `MoveClip`s as one undo step), and
  `UndoStack` (execute/undo/redo, dirty-flag tracking for the window title).
- `src/core/xml/` — `saveProject()`/`loadProject()`: the project file is
  valid MLT XML with `ustudio:`-namespaced properties carrying the model
  losslessly (ADR-004), written by this project's own serialiser, not
  MLT's `xml` consumer. See "Project files" below.
- `src/core/log.{h,cpp}` (`ustudio::core::Log`) — the logging module, thread-
  safe, no dependencies. Writes to `$XDG_STATE_HOME/ustudio/logs/`.
- `src/engine/factory_policy.{h,cpp}` (`ustudio::engine::FactoryPolicy`) —
  owns `Mlt::Factory::init()`/`close()` for the whole process: exactly one
  instance, constructed in `main()` before any window or
  `PlaybackController`, destroyed after `g_application_run()` returns.
  Builds a curated MLT module directory under
  `$XDG_RUNTIME_DIR/ustudio-mlt-modules/` (or, when `XDG_RUNTIME_DIR` isn't
  set — CI containers and other headless environments don't have a logind
  session — under the system temp directory instead) containing symlinks
  to every module except a `qt6`/`glaxnimate-qt6` denylist, so
  `Mlt::Factory::init()` never `dlopen`s Qt6 — a plain, argument-less
  `Factory::init()` measurably does (32 Qt library mappings observed on
  this machine); the curated approach was verified, via a standalone
  repro, to give zero Qt mappings while every consumer/transition this app
  needs (including `sdl2_audio`/`rtaudio`) stays available. See ADR-007.
  `PlaybackController` itself never calls `Factory::init()`/`close()` —
  doing so from two places would silently re-run init with the wrong
  (default) directory the second time.
- `src/engine/engine_sync.{h,cpp}` (`ustudio::engine::EngineSync`) —
  subscribes to `Model::changed` and projects the model into an
  `Mlt::Tractor` (ADR-003/005): rebuilds every track's playlist from
  scratch on any edit (coalesced to one rebuild per batch/composite
  command), verified in debug builds/tests against the model
  (`EngineSync::verify()`). Also builds the throwaway `Profile`/`Tractor`
  pair `renderProject()` and asset-length probing use.
- `src/engine/playback_controller.{h,cpp}`
  (`ustudio::engine::PlaybackController`) — owns an `Mlt::Consumer`
  (`sdl2_audio` → `rtaudio` → `null`, ADR-002) and drives playback from its
  `consumer-frame-show` event instead of a hand-rolled pull loop. See
  "Playback engine notes".
- `src/engine/dispatcher.{h,cpp}` (`ustudio::engine::MainThreadDispatcher`)
  — posts a closure from any thread onto the GLib main thread
  (`g_main_context_invoke_full`), guarded by a lifetime token so a post
  outliving its owner is dropped instead of touching freed state. Used by
  `PlaybackController` to get decoded frames from the consumer's thread to
  the GTK main thread.
- `src/engine/waveform_cache.{h,cpp}` (`ustudio::engine::WaveformCache`) —
  a small, separate MLT touchpoint (opens its own throwaway
  `Profile`/`Producer` per clip) for background audio-peak extraction,
  kept outside the live tractor/consumer specifically so it never contends
  with or blocks editing/playback.
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

- **Consumer-based, not pull-based** (ADR-002). `PlaybackController` owns
  an `Mlt::Consumer` (`sdl2_audio` → `rtaudio` → `null`, tried in order via
  `is_valid()`; all three confirmed present under `FactoryPolicy`'s curated
  module directory with a standalone repro) and reacts to its
  `consumer-frame-show` event rather than pulling frames on a hand-rolled
  thread. This removed the old pull loop's constant ~150ms A/V offset,
  drift on silent clips, and inability to drop frames — see
  [`docs/plans/v2/00-v1-review.md`](docs/plans/v2/00-v1-review.md) for what
  it replaced.
- **The frame-show handler runs on an MLT-owned thread, not the main
  thread.** It does the minimum possible — copy the frame into a
  mutex-guarded single-slot mailbox, post a coalesced wakeup via
  `MainThreadDispatcher` — and touches nothing else. Everything else on
  `PlaybackController` (play/pause/seek/...) is main-thread-only, so unlike
  the old `MltEngine` there is no project-wide mutex.
- **Pause is speed 0 + purge + refresh, not "stop pulling".** Set
  `tractor.set_speed(0)`, then `consumer.purge()` (flush the prefetch
  buffer) and `consumer.set("refresh", 1)` (force exactly one frame
  through) — the pattern kdenlive uses for frame-accurate pause. A plain
  speed-0 with no purge would keep showing whatever was already prefetched
  ahead of the playhead.
- **Position source of truth**: the position carried by each
  `consumer-frame-show` event while playing, the last explicit seek target
  while paused. Never `tractor->position()` for display — it runs ahead of
  what's on screen by the consumer's prefetch buffer.
- **A running consumer is reconnected in place on every ordinary edit, not
  restarted.** `EngineSync` rebuilds the tractor as a new object after
  every edit (see below); `PlaybackController::setTractor()` detects
  whether the new tractor's `Mlt::Profile*` is the same object the running
  consumer was built against (the common case — same session, same
  profile) and just calls `consumer.connect(newTractor)` if so, so editing
  never clicks or drops the audio device. Only an actual profile change
  (a project load with a different resolution/fps) stops and re-selects
  the consumer from scratch.
- **Multi-track audio does not mix by default.** An explicit `"mix"`
  transition is required between tracks, and it needs `start=1` (constant
  full level, not a crossfade) *and* `sum=1` (the default halve-then-add
  algorithm measured no different from not mixing at all in testing).
  `"composite"` (video) needed no such tuning — full-frame top-track-wins is
  its default with no geometry configured.

### Engine sync notes

`EngineSync` rebuilds a track's whole MLT playlist from the model on any
change (clear it, re-append blanks and cuts in position order) rather than
doing incremental playlist surgery (ADR-005) — simpler and always
consistent by construction, at the cost of being O(clips on the track) per
edit instead of O(1); acceptable at today's scale per
[doc 13's risk R5](docs/plans/v2/13-risks-and-open-questions.md). Cuts share
a per-asset master producer (`Mlt::Producer::cut()`), so no source file is
reopened per clip. `playlist.get_clip()`'s own `resource` property on a cut
is the placeholder string `"<producer>"`, not the real file path — that's
on the master producer / the model's own `Asset::path`, confirmed
empirically with a standalone repro. A clip's `videoEnabled`/
`audioEnabled` flags (used by "Split Audio") are applied per cut, not on
the shared master producer — `cut->set("video_index", -1)` /
`set("audio_index", -1)` (`-1` = "off", confirmed against `avformat`'s own
YAML metadata) — so silencing one clip's video or audio never affects any
other cut of the same asset elsewhere on the timeline.

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

It renders from a completely separate, throwaway `EngineSync` (its own
`Profile`/`Tractor`) built from a deep copy of the `core::Model` taken
synchronously on the main thread before the render thread starts — not a
reference to the live, editable model, which `UndoStack::execute()`/
`undo()`/`redo()` mutate in place on the main thread with no lock. A render
(which can take real encode time for a long project) therefore never races
an edit and never blocks editing or playback while it runs. The sequence
profile itself is a plain numeric width/height/fps/etc. (`core::Profile`,
doc 03), not a fixed MLT stock profile name; its default matches this
project's working format (1920×1080/30fps).

### Project files

Save/load uses **MLT XML with `ustudio:`-namespaced properties** (ADR-004),
not a bespoke format — `.ustudio` files are valid MLT XML that `melt` or
`u-studio-render` can play with no editor involved, because the
tractor/playlist/track structure is a real MLT graph. Earlier (v1) this was
a small `GKeyFile`-format file instead, because MLT's own `xml`
consumer/producer round-trip doesn't give back an editable structure (the
reloaded XML producer reports `Service::type() == mlt_service_producer_type`,
not `mlt_service_tractor_type` — wrapping it as `Mlt::Tractor` silently
yields zero tracks and segfaults on playback, confirmed empirically). The
fix wasn't to give up on MLT XML, but to stop depending on MLT's own
reader for project *state*: `core/xml/writer.cpp` writes the MLT graph
(for `melt`/other tools) *and* the full model losslessly in `ustudio:*`
properties on the same nodes; `core/xml/reader.cpp` reads back only the
`ustudio:*` properties and ids, ignoring the MLT graph entirely — the MLT
structure is regenerated by `EngineSync` on load, never read as input. See
[doc 09](docs/plans/v2/09-persistence-and-formats.md) for the full format
and the autosave/recovery scheme.
