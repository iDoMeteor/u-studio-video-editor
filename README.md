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
  track space for a per-track volume slider, "Lock Track"/"Unlock Track",
  and "Remove Track". A locked track refuses insert/move/resize/split/
  remove on its own clips (and as a move/Split Audio destination) until
  unlocked — reordering the track itself and toggling the lock stay
  available. Locked rows get a subtle tint so you can tell at a glance.
- Per-clip audio waveforms, drawn on every clip that has audio (video or
  audio-only), computed on a background thread so the UI never stalls —
  see `src/engine/waveform_cache.{h,cpp}`.
- Render the project to an MP4 matching this project's fixed working
  format (H.264 High/yuv420p, 1920×1080, 30fps, AAC 48kHz stereo) via the
  header bar's "Render…" button. Runs on a background thread.
- Save/load a project as MLT XML with `ustudio:` namespaced properties
  (see "Project files" below), plus autosave and crash recovery. A Reload
  button (refresh icon) re-opens the current project's file from disk
  without a file-picker round trip; a New Project button resets to a
  fresh, empty, untitled project. Neither touches what's on disk beyond
  what Reload reads.
- Timestamped debug/info/warn/error logging to
  `$XDG_STATE_HOME/ustudio/logs/` (falls back to
  `~/.local/state/ustudio/logs/` if `XDG_STATE_HOME` is unset), level
  configurable via `USTUDIO_LOG_LEVEL` (`debug`/`info`/`warn`/`error`/`none`,
  default **`debug`** while this app is actively being debugged — set
  `USTUDIO_LOG_LEVEL=info` for quieter logs once things have settled.
  Every user-facing status message (`AppWindow::
  showStatus()`, covering import/save/open/render/split/close-gap/lock/
  volume outcomes) is logged at debug level automatically, and the
  playback engine's consumer lifecycle (select/start/stop/restart) and
  `EngineSync::rebuildAll()`/`reset()`/`renderProject()`/waveform decode
  jobs log their own timing via `Log::ScopedTimer` (`core/log.h`) — run
  with `USTUDIO_LOG_LEVEL=debug` to get a full trace of what the app did
  and how long each step took, for both bug reports and performance
  outliers.
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
- **Every `setTractor()` call is a full stop/reselect/restart of the
  consumer — never `Mlt::Consumer::connect()` on one that's already
  running.** `EngineSync` rebuilds the tractor as a new object after every
  edit (see below), and an earlier version of `PlaybackController` tried
  reconnecting the live consumer to the new tractor in place for the
  common case (same profile) to avoid closing and reopening the real audio
  device on every edit. That turned out to corrupt MLT's internal state:
  reproduced 3/3 with a GDB backtrace crashing inside MLT's own
  `consumer_read_ahead_thread`/`mlt_service_get_frame`, sometime after the
  swap, reading through memory belonging to the tractor that had just been
  replaced — its background read-ahead (prefetch) thread was still running
  against the old one when the swap happened. Paying for a device
  close/reopen on every edit is the actual cost of the safe version;
  see `tests/engine/test_playback_controller.cpp`'s regression test for
  the exact scenario.
- **Exactly one `AppWindow` (and therefore one `PlaybackController`) for
  the whole process, enforced, not just assumed.** `G_APPLICATION_DEFAULT_
  FLAGS` makes this app single-instance, so GApplication redelivers the
  `"activate"` signal to the *already-running* primary instance every time
  something else tries to launch it again (a second double-click, a
  second terminal invocation) — confirmed from a real session's log,
  `main.cpp`'s `onActivate()` used to build a brand new `AppWindow` on
  every one of those instead of presenting the existing one, silently
  leaving an earlier `PlaybackController` (and its live `sdl2_audio`
  consumer) running and orphaned alongside a second one in the same
  process. Two `sdl2_audio` consumers fighting over the same PipeWire
  client state in one process is the leading suspect for a real SIGSEGV
  captured inside MLT's SDL2 audio callback thread, and matches
  "playback stopped working after relaunching once already running."
  Fixed by checking for an existing window first and presenting it
  instead — the standard GtkApplication pattern for a single-window app.
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
other cut of the same asset elsewhere on the timeline. Per-track volume
(`Track::volume`, a linear 0..1 scale for the UI) is applied by attaching
an `Mlt::Filter("volume")` to the track's playlist and converting to the
filter's own `"level"` property (dB — `"gain"` is documented deprecated
in its YAML metadata) with the standard `20*log10` amplitude-ratio
formula; confirmed with a standalone repro that -20dB measures a 0.1x
peak-amplitude ratio, exactly as expected, and again against the real
engine pipeline in `tests/engine/test_engine_sync.cpp`.

### Waveform cache notes

`WaveformCache` opens its own throwaway `Mlt::Profile`/`Producer` per clip
(never the live tractor's), built with the **sequence's own frame rate**,
not a hardcoded stock profile. Confirmed empirically that this matters:
`Mlt::Producer::get_length()`/`seek()` are normalized to whichever
`Mlt::Profile` the producer was constructed against, not the source file's
native rate — the same file opened at 25fps vs. 50fps reported
`get_length()` of 928 vs. 1857 (not the same number, roughly double,
matching the fps ratio). A clip's `in`/`out` are stored in the sequence's
own fps, so extracting peaks with a mismatched profile would silently seek
every job to the wrong wall-clock position for any project that isn't
exactly that rate. The peak cache is keyed on `(resource, in, out, fps)`
for the same reason — a later project at a different rate must never reuse
another rate's peaks for what would otherwise look like the same clip.

Peak count is capped at 2000 per clip regardless of its length, decoding a
stride of frames instead of every single one above that — `drawWaveform()`
(`app_window.cpp`) already re-buckets whatever's in the peaks array down to
the clip's actual on-screen pixel width, so one peak per video frame on a
long clip was always more resolution than anything ever displayed. Measured
on a real ~62-minute (110,854-frame) recording: 18.2 seconds decoding every
frame before this fix, 2.5 seconds after (stride ≈ 55, ~1980 peaks) — and
that 18 seconds of one CPU core solidly decoding the same file the live
playback consumer was also trying to read from is the leading explanation
for "playback doesn't work" reports that turned out to be "playback is
starved for the fifteen-ish seconds after importing or editing a long
clip," not a hard failure.

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

The render itself is atomic and never overwrites source media (audit A6):
`AppWindow` refuses a Save or Render path that resolves to one of the
project's own bin assets, and `renderProject()` encodes to a `<path>.part`
sibling, renaming it onto the real target only after a successful run.
That rename is also the actual failure detector for one MLT quirk
confirmed empirically here: pointing the output at a directory that
doesn't exist leaves `Mlt::Consumer` reporting itself valid and
`consumer.run()` returning 0 ("success") even though `avformat` never
created the file — one more MLT return value CLAUDE.md's own
empirical-knowledge rule says not to trust at face value.

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

A loaded project file is untrusted input (CLAUDE.md): `loadProject()`
refuses a `<profile>` with a zero/negative frame rate outright, corrects a
missing or too-low `ustudio:next_id` up to the largest id actually present
rather than trusting it (a stale or hand-edited value would otherwise
collide a future id with one already in the file), and runs `Model::check()`
on the fully-parsed result before handing it back — a well-formed XML file
that still violates a model invariant (an out-of-range clip span, an
overlap, a clip on the wrong track) is refused with a specific reason
rather than silently loaded.

Recovered content stays marked unsaved (`UndoStack::markDirty()`) rather
than looking identical to a freshly-saved project, and its autosave file
is only deleted once a manual Save actually lands the recovered work
somewhere durable — not the moment recovery finishes, which would leave a
second crash before that Save with no copy of the work at all. Each
autosave's `.meta` sidecar also records its writer's pid: a still-running
instance's own in-progress autosave is never offered (or discarded) by a
different instance's recovery dialog, since `findRecoverable()` skips any
entry whose recorded pid is still alive.

Save and Render both refuse a chosen output path that resolves to a file
already in the project's own media bin — CLAUDE.md's "never overwrite a
user's source media" rule, enforced rather than just followed by
convention. `renderProject()` also encodes to a `<path>.part` sibling and
renames it onto the real target only once the render succeeds, which
turned out to be the only reliable way to detect one MLT failure mode:
pointing the output at a directory that doesn't exist leaves
`Mlt::Consumer` reporting itself valid and `consumer.run()` returning 0
("success") even though `avformat` never created the file — confirmed
empirically, and consistent with this project's working assumption that
MLT return codes aren't trustworthy on their own.
