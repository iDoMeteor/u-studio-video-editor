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
  trim-to-fit needed. Import also probes fps and pixel dimensions (from the
  media's own `meta.media.*` properties) and records the file's format
  (extension) for the media browser below.
- Collapsible media browser panel to the left of the video preview (toggle
  from the header-bar button next to "Add track"): every imported asset as
  a row with a thumbnail, name, length, fps, and format. Thumbnails are
  decoded as background jobs on the worker pool
  (`src/engine/thumbnail_cache.{h,cpp}`, the same architecture as the
  waveform cache below) so importing a large file never blocks the UI. Right-click a row for "Remove from Project"
  (drops the asset and every clip cut from it, undoable — the file on
  disk is untouched) or "Move File to Trash…" (confirms, since removing
  it from the project isn't chained onto the file move by the undo stack,
  then does both: removes it from the project and moves the actual file
  to the desktop's Trash via `g_file_trash()` rather than permanently
  unlinking it — audit A3, 2026-09-23 — so it's still recoverable the
  normal way). Drag a row onto the timeline to insert a full-
  length clip at the exact track/frame the drop lands on — refused, like
  any other insert, if that space isn't free or the track is locked.
  Double-click a row to insert it at the playhead on the active track
  instead. Drag files in from outside the app (the file manager, most
  likely) onto the timeline to import and place them one after another
  starting at the drop point, or onto the media browser to just add them
  to the bin with no clip placed. Import (`Ctrl+I`) supports selecting
  several files at once, each landing after the previous one on the
  active track. Files are probed in parallel off the main thread, with
  "Importing 3 of 12…" in the status bar, and a file that can't be opened
  is reported and skipped without stopping the rest. Both Import and Open Project filter their file
  pickers to media files and `.ustudio` projects respectively.
- Multi-track timeline: add/remove tracks, drag a track's handle to reorder
  it, click a row to make it the active track (where imports/splits land).
  Higher tracks composite over lower ones for video (full-frame, top wins);
  all tracks mix together for audio, including tracks that are audio-only.
  A timecode ruler runs along the top, ticking every 1/2/5/10/15/30
  seconds or whole minutes/hours — whichever keeps ticks at least ~60px
  apart at the current zoom (enhancement #12, 2026-09-23).
  Zoom with Ctrl+mouse wheel (the frame under the pointer stays put) or
  `+`/`-`, and `0` to fit the whole project; scroll sideways with
  Shift+wheel, a touchpad swipe or the scrollbar under the tracks, and
  vertically when there are more tracks than fit. The view follows the
  playhead a page at a time. The zoom/scroll maths is
  `src/app/timeline/viewport.{h,cpp}` (doc 06's Viewport), unit-tested.
- **Undo/redo** for every edit (header-bar buttons, `Ctrl+Z`/`Ctrl+Shift+Z`),
  backed by a real command/undo-stack model — see "Architecture" below.
- Playback via an MLT consumer (`sdl2_audio`, falling back to `rtaudio`,
  then `null` — see "Playback engine notes"): play/pause (`Space`, or the
  play button in the transport bar), `J`/`K`/`L` shuttle (repeated `J`/`L` ramps speed
  1x→2x→4x→8x), frame step
  (`Left`/`Right`; `Ctrl+Left`/`Ctrl+Right` for 10 frames, `Alt+Left`/
  `Alt+Right` for a minute, both clamped to the timeline's start/end),
  jump to start/end (`Home`/`End`), loop in/out (`I`/`O` at the playhead),
  volume, preview-scale preference (Auto/Full/Half/Quarter), scrub by
  dragging the seek bar (including mid-playback) or by clicking and
  dragging directly on empty timeline space, which follows the pointer
  continuously rather than only seeking once on release (enhancement
  #11, 2026-09-23). A vertical cyan line on
  the timeline itself marks the current frame across every track, live
  during playback, not just the seek bar below the preview -- drawn on
  its own overlay layer so updating it 30 times a second doesn't also
  redraw every clip/waveform/label underneath (audit A5, 2026-09-23).
  `A`/`F` jump the playhead to the previous/next cut (a clip's start or
  end, or the timeline's own start/end) on any track, and `Shift+A`/
  `Shift+F` to the previous/next cut on the active track only. `S`/`D` move
  which track is active up/down, the same target a plain click sets.
  `Shift+S`/`Shift+D` move a single selected clip to the nearest track
  above/below where it fits at the same position (same kind of track;
  tracks where it would overlap a clip, or locked ones, are skipped; a
  clip in a dissolve is refused), or with nothing selected jump the
  active track to the top/bottom.
- Split the active track's clip at the playhead (`X`, or the scissors
  button in the transport bar).
- Transport buttons around the play button: go to start, shuttle reverse,
  step back one frame, play/pause, stop, step forward one frame, shuttle
  forward, go to end. Each is bound to the same window action as its
  keyboard shortcut (tooltips name the key).
- Drag a clip's body to move it — within a track or to a different one.
  Drag near a clip's left/right edge (~8px) to trim it shorter or longer.
  Both snap, within that same ~8px, to clip edges on every track (never
  the dragged clip's own), markers, the playhead and frame 0, on whichever
  edge of the dragged clip is closer; a magenta line shows the snap. The
  gesture logic is `src/app/timeline/timeline_controller.{h,cpp}` (doc
  06's TimelineController), tested without GTK.
- Select several clips: Shift+click adds one, Ctrl+click toggles one,
  Shift+drag across empty track space selects everything the box touches,
  `Ctrl+A` selects all and `Escape` clears. Drag any selected clip to move
  the whole selection together (one undo step, `core::MoveClips`); `Delete`
  removes every selected clip.
- Ripple, slip and copy: Alt+drag a clip's edge to ripple trim (later
  clips on the track follow), Shift+drag an edge to slip (same place and
  length, different source frames; stops at the ends of the source),
  Ctrl+drag a clip to copy it (a Ctrl+click only toggles the selection),
  and `Shift+Delete` to ripple delete the selection. `M` adds a marker at
  the playhead and `Shift+M` removes it; markers show on the ruler and
  edges snap to them. The commands are in `src/core/commands/
  timeline_edits.h`.
- Ripple mode (`R`, or the Ripple button in the transport): while it's on,
  moving a clip closes the gap it leaves and pushes later clips along where
  it lands (`core::RippleMove`); a drop inside another clip goes to its
  nearer edge.
- Keyboard-only editing: `Tab`/`Shift+Tab` select the next/previous clip on
  the active track and move the playhead to it, `Up`/`Down` (as well as
  `S`/`D`) change the active track, `,`/`.` nudge the selection a frame
  (`Shift` for ten). Pinch to zoom on a touchpad or touchscreen.
  Right-click a clip, or click to select it then press `Delete`, to
  delete it (leaves a gap — "lift", nothing else moves); right-click a
  clip that has audio and isn't already audio-only
  for "Split Audio" (pulls its audio out to a new, independent clip on the
  nearest audio track with room, creating one if none has room — the two
  halves can then be moved/trimmed independently); right-click a gap to
  close it (ripples later content earlier to fill it); right-click empty
  track space for a per-track volume slider, "Lock Track"/"Unlock Track",
  "Hide Track"/"Show Track" (video tracks: the picture is off in preview
  and render, and the tracks under it show through), "Mute Track"/
  "Unmute Track", "Edit Track Name" and "Remove Track". The row's name
  strip says "Hidden"/"Muted" while either is on. A locked track refuses insert/move/resize/split/
  remove on its own clips (and as a move/Split Audio destination) until
  unlocked — reordering the track itself and toggling the lock stay
  available. Locked rows get a subtle tint so you can tell at a glance.
- Per-clip audio waveforms, drawn on every clip that has audio (video or
  audio-only), computed as pool jobs so the UI never stalls — see
  `src/engine/waveform_cache.{h,cpp}`. Video clips carry a strip of
  thumbnails edge to edge, frame-accurate, from their own cache (newest
  requests first; `ThumbnailCache::frameThumbnail`), with the waveform in
  the bottom 40% under them.
- The timeline is one custom widget (`UsTimelineView`, ADR-008) drawn in
  `snapshot` with GSK nodes by `src/app/timeline/timeline_renderer.
  {h,cpp}`: 10 tracks × 500 clips on screen snapshot in about 0.6 ms, and
  about 2 ms zoomed in with labels and waveforms (optimised build;
  `tests/app/test_timeline_render.cpp`). Its colours come from
  `style.css` through `tools/gen_tokens.py` (a generated `tokens.h`), so
  the drawn timeline and the CSS chrome share one set of tokens. A move or
  copy that would be refused draws red while you drag. Drop-ins can paint
  over the tracks through `AppWindow::addTimelineOverlay()` (ADR-013).
- Dissolve transitions between two adjacent clips on the same track,
  shown as a diagonal-hatch region on the timeline. Create one by
  dragging a clip's edge past its exactly-touching neighbor (the same
  trim-drag gesture as an ordinary trim — the drag distance becomes the
  transition's length), or by right-clicking near where two touching
  clips meet for "Add Transition" (a default ~half-second length, split
  between both clips' handles). Once created, drag either edge of the
  hatch region to grow or shrink it, or right-click it for "Remove
  Transition". Creating or resizing a transition grows/shrinks each clip
  using its own existing source-media handle, so the pair's combined span
  on the track never changes and nothing else needs to move. Undoable
  like any other edit, and played back as a real cross-fade (not just a
  model concept) — see "Engine sync notes" below.
- Name tracks and clips. Double-click a track's name strip (just above the
  row) or a clip to edit its name inline (Enter or click away to commit,
  Escape to cancel); right-click a clip for "Add Name"/"Edit Name" and,
  once it has one, "Remove Name". Clips cut from the same source can carry
  different names. A named track's name is drawn in the top-left corner of
  every clip on it. Hovering a clip shows a tooltip with its name (or
  "(unnamed)"), start/end timecodes, length (timecode and frame count),
  and source file.
- Render the project to an MP4 at the sequence's own size and frame rate
  (1920×1080, 30fps by default): H.264 (libx264, or libopenh264 where
  ffmpeg lacks it), yuv420p, AAC 48kHz stereo, via the header bar's
  "Render…" button. Runs on a background thread, with a live
  percentage in the status bar while it runs (enhancement #13,
  2026-09-23).
- Save/load a project as MLT XML with `ustudio:` namespaced properties
  (see "Project files" below), plus autosave and crash recovery. Autosave
  fires 2 minutes (configurable) after the last edit, on focus loss, and
  whenever the oldest unsaved edit reaches that age. So a crash or
  `kill -9` loses at most 2 minutes of work even while you edit
  continuously.
  `Ctrl+S` saves straight back to the project's own file with no dialog
  once it has one (falling back to the Save As dialog for an untitled
  project); `Ctrl+Shift+S` always opens the Save As dialog. `Ctrl+O`/
  `Ctrl+N`/`Ctrl+I` mirror the header-bar Open/New Project/Import
  buttons. A Reload button (refresh icon) re-opens the current project's
  file from disk without a file-picker round trip; a New Project button
  resets to a fresh, empty, untitled project. Neither touches what's on disk beyond
  what Reload reads. Open, Reload, and New Project all confirm first
  ("Discard unsaved changes?") if there are any (audit A2; Open joined
  them in audit A4, 2026-09-23 — navigating its file-picker dialog to
  choose what to open says nothing about the *current* project being
  discarded, so it was never the implicit confirmation it looked like).
  A "Recent projects" button (clock icon, next to Open) lists the last
  few `.ustudio` files (10 by default, set in Settings) opened or saved, backed by `GtkRecentManager` (so
  it also shows up in the GNOME Shell's own "recent files" if the desktop
  surfaces those) — picking one confirms unsaved changes first too, the
  same as Open. The window title shows the current project's name (or
  "Untitled Project") with a `•` while there are unsaved changes.
  Closing the window itself
  confirms too, with a third option: "Save changes before closing?"
  offers Save/Discard/Cancel, and Cancel leaves the window open exactly
  as it was (audit A2, 2026-09-23) — previously the window closed
  immediately with no prompt at all. Discarding, or any other path that
  reaches the app's own shutdown while still dirty, still leaves a final
  autosave behind as a recovery point, the same as any other still-dirty
  exit. Opening, reloading and recovering read the project file in the
  background too; opening another project while one is still loading
  replaces it. Saves and autosaves write in the background, so the window never
  freezes on a big project. Closing or quitting while a save is still
  writing waits for it to finish. An edit made during a save stays marked
  unsaved, because the file doesn't have it yet.
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
  `EngineSync::rebuildAll()`/`reset()`, `engine::renderProject()`, waveform decode
  jobs log their own timing via `Log::ScopedTimer` (`core/log.h`) — run
  with `USTUDIO_LOG_LEVEL=debug` to get a full trace of what the app did
  and how long each step took, for both bug reports and performance
  outliers.
- No Qt/KDE anywhere in the *running process*, not just the link line —
  `FactoryPolicy` curates the MLT module directory at startup so Qt6's MLT
  modules never get `dlopen`'d. See "Architecture" below.
- No PulseAudio/`libpulse-simple` dependency — audio output goes through
  the same MLT consumer as video.
- Help dialog (header-bar `?` button): a Controls tab describing every
  button, menu item and timeline gesture, a Keyboard Shortcuts tab listing
  every action and its default accelerator, grouped by category (Playback/
  Editing/Project), and an About tab. The Controls tab and every control's
  tooltip come from one table, `src/app/ui_hints.{h,cpp}`: a hint names
  its action and the shortcut is looked up in `action_registry.cpp`, so a
  tooltip can't show a stale key. Drop-ins add their own hints with
  `registerHints()`. Settings dialog (header-bar gear
  button): General (autosave delay, recent-projects list size, worker
  threads) and
  Playback (default preview scale, maximum shuttle speed) tabs, backed by
  real `GSettings` persistence (`data/com.ustudio.VideoEditor.gschema.xml`)
  — falls back to in-memory defaults with a one-time warning log and an
  in-dialog toast if the schema isn't installed/compiled (the common case
  when running straight from `builddir` without `meson install` — export
  `GSETTINGS_SCHEMA_DIR=<builddir>/data` or use `meson devenv -C builddir`
  to exercise real persistence during development). A placeholder Keyboard
  Shortcuts tab in Settings, and the shared `src/app/action_registry.h`
  table both the Help tab and `installActions()` read from, are the
  intended foundation for a future hotkey-rebinding feature.

Not yet: effects, titling, proxy/transcode, configurable export formats
(render always uses the settings above — see "Render implementation
notes" below), marker names (markers are unnamed for now), a visible loop
region.

## Roadmap

Active development is following a v2 rewrite plan recorded under
[`docs/plans/v2/`](docs/plans/v2/) — architecture, project model, milestones
(M0–M7), and the ADRs that are the binding contract for load-bearing
decisions. M0 (the `core/engine/app/render` restructure) and M1 (project
model, commands, undo/redo, MLT-XML save/load, autosave/recovery) have
landed; M2 (playback via a real MLT consumer instead of a hand-rolled pull
loop, per ADR-002) is built, with an automated A/V sync test
(`engine-av-sync`) and a soak tool (`tests/engine/playback_soak.cpp`); its
4K60 soak result is accepted. M3 (the multi-track timeline) is done,
pending the post-M3 audit. See
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

Sanitizer runs, each in its own build directory (`builddir-asan`,
`builddir-tsan`). Both pass clean, so any report they print is a real
finding:

```sh
just asan    # ASan + UBSan + LeakSanitizer over the whole suite
just tsan    # ThreadSanitizer over the whole suite
```

`tests/sanitizers/lsan.supp` lists the leaks that aren't ours (MLT's
loader and module repository, FFmpeg worker threads, SDL). Its header
explains why a leaked mlt++ wrapper of ours still reports through it (a
reintroduced `Tractor::track()` leak was confirmed to fail `just asan`).
The `justfile` comments explain the non-default sanitizer options each
recipe needs. First run and findings:
[`docs/audit/2026-09-23-sanitizer-report.md`](docs/audit/2026-09-23-sanitizer-report.md).

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
  multi-step edits (e.g. "close gap" = one `ShiftClips` as one undo step), and
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
  command), verified in tests against the model
  (`EngineSync::verify()`). Also builds the throwaway `Profile`/`Tractor`
  pair `renderProject()` and asset-length probing use.
- `src/engine/playback_controller.{h,cpp}`
  (`ustudio::engine::PlaybackController`) — owns an `Mlt::Consumer`
  (`sdl2_audio` → `rtaudio` → `null`, ADR-002) and drives playback from its
  `consumer-frame-show` event instead of a hand-rolled pull loop. See
  "Playback engine notes".
- `src/engine/dispatcher.{h,cpp}` (`ustudio::engine::MainThreadDispatcher`)
  — posts a closure from any thread onto the GLib main thread
  (`g_idle_add_full`, never inline on the posting thread), guarded by a lifetime token so a post
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
- `tests/core/`, `tests/engine/`, `tests/app/` — doctest suites (vendored under
  `subprojects/doctest/`, no system package needed). `tests/engine/`
  needs MLT but no display and no media files.

### Playback engine notes

**Playback and graph builds run on their own thread (0.28.0, doc 19 MT2).**
`engine::Engine` owns one engine thread, which holds `EngineSync`, the
tractor and `PlaybackController`. The window publishes a model snapshot per
edit, undo or redo; the engine thread rebuilds and restarts the consumer
without blocking the window. The window reads position, length and fps from
a mirror, and a seek or step moves that mirror immediately, so quick steps
still count. Consecutive snapshots collapse (latest wins), and a seek after
an edit lands on the new graph. The consumer's frame hand-off runs on the
engine thread, so loop wraps can seek; frames reach the window through a
one-slot hand-off. `Engine::shutdown()` stops the consumer and drops the
graph before `Factory::close()`. The engine thread logs its own entries over
50 ms at debug ("engine thread busy"); the main-thread stall monitor doesn't
see them.

**The playback consumer prerolls one frame (`prefill` = 1).** With a larger
preroll, MLT 7.40's consumer thread (waiting in `mlt_consumer_rt_frame()` for
min(prefill, buffer) frames) and its read-ahead thread (which stops at one
queued frame once it reads a paused frame) could both wait, and stopping
that consumer then hung the main thread in the sdl2 consumer's join
(post-M3 audit P3; 2/64 stress runs before, 0/256 after). The read-ahead
still fills to `buffer` behind playback.

**Preview scale works by shrinking the playback profile.** Setting the
consumer's `scale` property had no effect on what MLT renders (4K60 at
"Half" still delivered 3840×2160 frames and showed only 20–23 of 60 frames
a second), and overriding the consumer's width/height only added a final
downscale after full-size rendering (slower). `EngineSync` now builds the
playback tractor on the sequence profile scaled by the preview factor
(even dimensions, same fps and aspect); export keeps the full profile.
Auto means Half for sequences taller than 1080 lines. Details and
measurements: doc 05, "Preview scale". Leave the `avformat` producer's
`threads` unset: unset already decodes with about one thread per CPU, and
explicit values measured no better.

**SDL signal handlers are disabled.** MLT's `sdl2_audio` consumer
initialises SDL, and by default SDL turns SIGINT/SIGTERM into an
`SDL_QUIT` event that nothing in a GTK app reads, so `kill`, Ctrl+C and
session logout were ignored until the app was SIGKILLed (sanitizer report
S2, 2026-09-23). `main()` sets `SDL_NO_SIGNAL_HANDLERS=1` before any
consumer starts. It doesn't overwrite a value already in the environment.
Both signals are handled with `g_unix_signal_add()` → `g_application_quit()`,
so they take the normal shutdown path: final autosave if dirty, then the
consumer stops before `Factory::close()`.

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
- **Pause is speed 0 + seek back + purge + refresh, not "stop pulling".**
  Set `tractor.set_speed(0)`, **seek the tractor back to the frame last
  shown** (`consumer-frame-show`'s position), then `consumer.purge()`
  (flush the prefetch buffer) and `consumer.set("refresh", 1)` (force
  exactly one frame through) — the pattern kdenlive uses for frame-accurate
  pause. The seek matters: during playback the read-ahead thread has
  already pulled the producer up to `buffer` (25) frames past the screen,
  and `purge()` drops the queued frames without moving the producer back.
  Without it, pausing with frame 40 on screen showed frame 72 (audit E1;
  regression test "pausing mid-playback stays on the last displayed
  frame"). **`play()` must write `"refresh"` after setting speed back
  up** (it writes 0): while paused, `sdl2_audio`'s consumer thread shows
  one frame and then blocks on a condition variable that only a write to
  the `"refresh"` property wakes (`consumer_refresh_cb` in MLT's
  `consumer_sdl2_audio.c`; any write fires it, the value is irrelevant), so
  `set_speed()` alone left play() frozen at the paused position — one seek
  or the implicit `pause()` every `setTractor()` ends with was enough to
  trigger it. The `null` consumer has no such wait, which is why
  null-consumer tests never caught it.
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

**mlt++ accessors that return a pointer allocate a new wrapper — delete
it, whatever the header says.** `Mlt::Tractor::field()` and
`Mlt::Tractor::track(int)` are documented "caller does not own the
result", but each call returns a fresh wrapper holding its own reference on
the underlying MLT object. Calling `field()` inline without deleting it
leaked every transition planted in every rebuilt tractor, ~100–330 KB per
edit (sanitizer report S1, 2026-09-23; confirmed with
`docs/audit/2026-09-23-sanitizer-run/rebuildrepro.cpp`: RSS +10 MB per 100
rebuilds leaked, flat when deleted, and ASan-clean either way). Hold these
in a `std::unique_ptr`.

**Building a playlist entry by entry is O(n²).**
`mlt_playlist_append()` and `blank()` end in
`mlt_playlist_virtual_refresh()`, which walks every entry doing three
locked property lookups each (`mlt_playlist.c`, 7.40). With 5,000 clips on
one track a rebuild took 17–21 s. MLT has no batch or deferred-refresh
API. So `EngineSync::rebuildTrackPlaylist()` builds a track with more than
64 entries from nested sub-playlists of 64, each marked `ustudio.chunk`.
That's linear, and frame-, audio- and render-identical to the flat graph
(`engine-chunked-playlist`). `verify()` flattens the chunks back. A track
of 64 entries or fewer stays flat, exactly as before (doc 19, MT2 piece 0).

**Every `mix` transition is a 9.2 MB `calloc()`, so pin glibc's mmap
threshold.** `transition_mix.c` embeds two 192,000-sample × 6-channel
float buffers. There is one per track and one per dissolve. glibc
serves them from fresh, already-zero mmap pages until its dynamic mmap
threshold climbs past 9.2 MB. It does that the first time one is freed,
i.e. on the first rebuild. After that each one comes from the heap and
is zeroed in full. On a 5,000-clip project with 1,992 dissolves, rebuilds
went from 0.35 s to 5.7 s and resident memory reached 36 GB within nine
rebuilds. `FactoryPolicy` sets `M_MMAP_THRESHOLD` to 4 MiB before
`Factory::init()`, which also disables the dynamic adjustment. Rebuilds
then stay at 0.3 s and 400 MB. `engine-sync`'s "resident memory" test
fails without it (+5.4 GB).

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
`audioEnabled` flags (used by "Split Audio") are applied through a
separate master producer per (asset, video on/off, audio on/off)
combination, with `video_index`/`audio_index` set to `-1` ("off", per
`avformat`'s own YAML) on that master. **Not on the cut:** MLT ignores both
properties on a cut, confirmed with a standalone repro (2026-09-24). A cut
with `audio_index=-1` still played at full level, while the same property
on its master silenced every cut taken from it. Until then, a Split Audio
clip's video half kept playing its sound underneath the new audio clip.
Each clip's own variant means silencing one clip never affects another
cut of the same asset. Per-track volume
(`Track::volume`, a linear 0..1 scale for the UI) is applied by attaching
an `Mlt::Filter("volume")` to the track's playlist and converting to the
filter's own `"level"` property (dB — `"gain"` is documented deprecated
in its YAML metadata) with the standard `20*log10` amplitude-ratio
formula; confirmed with a standalone repro that -20dB measures a 0.1x
peak-amplitude ratio, exactly as expected, and again against the real
engine pipeline in `tests/engine/test_engine_sync.cpp`.

A dissolve transition between two adjacent same-track clips
(`core::Transition`, `AddTransition`/`RemoveTransition`) is built as a
small 2-track `Mlt::Tractor` (clip `a`'s tail on track 0, clip `b`'s head
on track 1, connected by `luma` + `mix`) nested as one entry inside the
track's own playlist — `core::planTrackSegments()` (shared with the XML writer) is the single
source of truth both `rebuildTrackPlaylist()` and `verify()` build/check
against, so they can't drift. Both the `luma` and `mix` transitions
**must** have their own `in`/`out` set explicitly to the sub-tractor's
local `[0, length)` range — confirmed empirically (2026-09-22) that
leaving them unset corrupts the video dissolve into flat garbage colour
for the last couple of overlap frames, but *only* once track 0's cut
producer has the non-zero absolute `in` a real clip's tail always has (a
toy zero-based repro is not enough to catch this). `mix` additionally
needs `start=-1` ("automatic linear crossfade", per its own YAML) rather
than the `sum=1, always_active=1` config used for the permanent
cross-track audio blend above — the YAML documents `sum` as incompatible
with `start < 0`, confirming the two uses need different settings; the
crossfade's audio correctness rests on that documented semantics plus the
same explicit-in/out fix, not an independent sample-level measurement (an
RMS probe on two `tone:` generators wasn't discriminating enough either
way). See `EngineSync::buildTransitionSubTractor()`'s comment and
`tests/engine/test_engine_sync.cpp`'s dissolve test (pixel-samples the
actual composited output) for the full finding.

A `core::Transition` assumes its two clips keep exactly the geometry
`AddTransition` gave them; nothing else used to know it existed. A
2026-09-22 audit found that removing, moving, resizing, or splitting
either linked clip left the transition dangling or pointing at the wrong
span — a crash on the next right-click/drag on that row (an unguarded
`m_model.clip(t.a)`/`clip(t.b)` in the timeline's right-click and
drag-begin handlers), and a saved project that fails `loadProject()`'s
own `check()` refusal on reopen. `RemoveClip`, `MoveClip`, `ResizeClip`,
`SplitClip`, and `RemoveTrack` each strip a transition touching the
clip(s) they're about to change first (`Model::removeTransition`, which
un-extends both linked clips back to pre-dissolve geometry) and restore
it on revert (`Model::addTransition`) — the clip simply loses its
dissolve, the same outcome a manual "Remove Transition" then the edit
would have produced. `SplitAudio` is deliberately untouched: it only
toggles a clip's video/audio-enabled flags, never its position/in/out, so
a transition on it stays geometrically valid throughout. The same audit
found a second, narrower bug (a clip linked on both sides at once — the
middle of an A-dissolve-B-dissolve-C chain — can have combined overlap
longer than its own length, which `AddTransition`'s single-transition
check doesn't catch but corrupts `planTrackSegments`'s layout); both
`AddTransition` and `Model::check()` now enforce it.

A 2026-09-23 follow-up audit found the T1 fix above was too eager:
`ResizeClip`/`SplitClip` stripped (or flatly refused) an edit touching
only a clip's *untouched* edge — trimming a clip's far tail away from an
incoming dissolve on its head, for instance, was refused outright,
because `Model::isRangeFree`'s overlap check only ever ignored the clip
being edited, not the still-present, still-legitimately-overlapping
partner clip a transition it wasn't stripping left in place.
`isRangeFree` now takes a set of ids to ignore (the clip itself, plus the
partner of any transition this edit determined it doesn't need to strip),
and `ResizeClip`/`SplitClip` strip only the transition(s) whose own
overlap region the edit would actually reach into — everything else on
the clip keeps its dissolve. `SplitClip` additionally repoints a
surviving *outgoing* transition from the original clip onto the new right
half (`Model::retargetTransitionClip`, which changes a transition's `a`/
`b` without touching either clip's geometry) rather than leaving it
attached to the half that no longer owns that edge. Two narrower bugs
from the same audit: `MoveClip::apply()` now refuses outright — before
touching the model at all — a "move" whose destination track and
position exactly match the clip's current ones (a click, or a small
same-row wobble the drag-vs-click pixel threshold didn't fully absorb),
since issuing it anyway would strip a dissolve for an edit that changes
nothing; and `RemoveAsset` now strips each of its clips' transitions
*before* capturing their (now pre-dissolve) geometry for revert, in the
same batch as the removal, so deleting an asset no longer leaves a
dangling transition on a clip about to disappear or on a surviving clip
it was linked to.

The inline track/clip name editor (`showInlineNameEditor()`, a `GtkPopover`
holding a `GtkText`) had the same problem an earlier audit found in the
preview-scale dropdown and the volume/seek sliders (audit A7, see their
`focusable=FALSE` comments in `app_window.cpp`): the transport actions
`installActions()` binds to bare letters and Left/Right/Home/End
(+Ctrl/Alt variants) are `win.*` accelerators installed via
`gtk_application_set_accels_for_action`, which fire as global window
shortcuts regardless of which widget has keyboard focus — typing "a" to
rename a track sought to the previous cut instead of inserting the letter
(audit A1, 2026-09-23). `GtkDropDown`/`GtkRange` could opt out by setting
`focusable=FALSE` (A7); a text entry can't, since it needs focus to accept
input at all. Fix: `showInlineNameEditor()` disables every one of those
`GSimpleAction`s (`g_simple_action_set_enabled`, looked up by name via
`g_action_map_lookup_action`) for the popover's lifetime, and
`onInlineNameEditClosed()` — wired to the popover's own `"closed"` signal,
which fires on every dismissal path (Escape, Enter, or clicking away) —
re-enables them unconditionally as its first statement. `GSimpleAction`
ignores `activate()` entirely while disabled (confirmed via a live gdb
session: `g_action_get_enabled()` read 0 for the duration the popover was
open, and a `win.step-forward` activation over D-Bus during that window had
no effect), so this holds even though this app doesn't control the order
GTK's shortcut controller and the entry's own key handling see the event.
Undo/redo (`Ctrl+Z`/`Ctrl+Shift+Z`) are deliberately left enabled: they're
modifier combos a text entry never needs to consume for itself.

The recent-projects menu (`refreshRecentProjectsMenu()`) hit two GLib/GTK
findings worth recording. `GtkRecentInfo` (what `gtk_recent_manager_get_items()`
returns) is its own refcounted boxed type with `gtk_recent_info_ref()`/
`_unref()` — **not** a `GObject`, despite looking like one; freeing the
returned list with `g_object_unref` as the element destructor segfaults
inside GObject's own type-check machinery on the very first real call
(confirmed live via gdb: `g_type_check_instance_is_fundamentally_a`),
not something a quick glance at the type name would catch. And calling
`gtk_recent_manager_get_items()` immediately after
`gtk_recent_manager_add_item()`, in the same call stack, does **not**
see the just-added item — confirmed live (the menu showed "No recent
projects" right after a save that had just added one) that
`GtkRecentManager` updates its in-memory list and emits `"changed"`
asynchronously, not synchronously inside `add_item()`. Fixed by
connecting `refreshRecentProjectsMenu()` to the manager's own
`"changed"` signal instead of calling it directly after `add_item()` —
the correct source of truth regardless of that timing, confirmed live
(the same save-then-check sequence then showed the entry correctly).
A third finding in the same area: sorting the recent-projects list with
`g_list_sort()` and a comparator that calls back into
`gtk_recent_info_get_modified()`/`g_date_time_compare()` on every
comparison crashed deep inside GLib's own `g_date_time_compare`
(`g_time_zone_get_offset`) against a large, real `recently-used.xbel`
history — reproduced twice against the owner's actual recent-files list,
never against this session's own small synthetic test histories. Fixed
by extracting each entry's modified time to a plain `gint64` once, up
front, and sorting a `std::vector<std::pair<gint64, GtkRecentInfo*>>`
with `std::sort` instead — no further GLib calls happen during the
comparison itself.

"Move File to Trash…" (`onDeleteAssetFileClicked()`) used to permanently
unlink the file with `std::filesystem::remove` despite the confirmation
dialog only asking about removing it from the *project* (audit A3,
2026-09-23) — fixed by moving it to the desktop's Trash with GIO's
`g_file_trash()` instead. Confirmed empirically (a standalone repro
calling the same GLib function directly) that this only succeeds for a
file on the same filesystem as `$XDG_DATA_HOME/Trash`: a file under
`/tmp` fails outright with `G_IO_ERROR_NOT_SUPPORTED` ("Trashing on
system internal mounts is not supported" — GIO treats tmpfs/internal
mounts as ineligible for trashing regardless of `$XDG_DATA_HOME`), and a
file on a genuinely different *device* from `$XDG_DATA_HOME/Trash` fails
with the same error code but "across filesystem boundaries" instead. A
real media file living under the user's home directory (this feature's
actual use case) hits neither case — confirmed by trashing a real file
there and finding it land at `$XDG_DATA_HOME/Trash/files/`, matching the
XDG trash spec. `onDeleteAssetFileClicked()`'s existing error path
(`showStatus()` with the failure message) already surfaces either error
to the owner rather than silently doing nothing, but a source asset that
somehow ends up on `/tmp` or a removable/network mount too small to hold
a Trash copy will report a failure here where the previous
`std::filesystem::remove` implementation would have just deleted it.

The timeline's playhead line used to be drawn as the last few lines of
`onTimelineDraw()` itself (`AppWindow::onTimelineDraw()`), so every
displayed frame during playback -- `refreshTransport()`, called once per
frame via `onFrameReady()` -- queued a full redraw of the whole timeline:
every track's Pango label layout, every clip's waveform-cache lookup
(mutex + key-string build), all ~30 times a second (audit A5,
2026-09-23). Fixed by splitting the playhead onto its own
`GtkDrawingArea` (`m_playheadOverlay`), stacked on top of `m_timeline`
inside a `GtkOverlay`, `gtk_widget_set_can_target(..., FALSE)` so it
never intercepts the clicks/drags/drops/tooltips every existing
controller is still attached to `m_timeline` for.
`refreshTransport()` now queues only that overlay; everything that
queues a full `m_timeline` redraw for an actual content change is
unchanged. Confirmed via gdb breakpoints on both draw functions that a
`step-forward` action now hits `onPlayheadOverlayDraw()` and *not*
`onTimelineDraw()`, while a real edit (adding a track) still hits
`onTimelineDraw()` as before -- GTK4's per-widget `GskRenderNode` caching
means the overlay's last-drawn playhead position stays correctly
composited on top even on a frame where only `m_timeline` redrew, so
nothing needed to force both together.

The timecode ruler (`onRulerDraw()`, enhancement #12, 2026-09-23) is its
own fixed-height widget stacked ABOVE `m_timeline` in the layout, not
overlapping it -- unlike the playhead overlay, it doesn't need to sit on
top of anything, so it's simplest as a separate widget rather than
another `GtkOverlay` child, and it never has to touch any of the row/
y-coordinate math `onTimelineClicked()`/`onTrackDragBegin()`/
`onTrackDragUpdate()`/`onTimelineRightClicked()` already do. One finding
worth recording: `gtk_widget_set_size_request()`'s height and the
widget's actual allocated height aren't always equal -- confirmed live
via gdb that a `kRulerHeight` of 20 requested came out as 18 actually
allocated (CSS padding from the shared `"timeline-area"` class both this
and `m_timeline` use). Anchoring the tick marks to the *requested*
constant instead of the real `height` parameter `onRulerDraw()` receives
would have drawn them a couple of pixels past the widget's real bottom
edge -- fixed by using `height` for the drawing math, keeping the
constant only for the original size request.

Fixing T1 surfaced a second, unrelated crash: `RemoveClip`/`MoveClip`/
`RemoveAsset` (and, before this fix, the un-batched T1 strip-then-edit
sequence) each performed two or more separate `Model` mutations with no
`BatchBegin`/`BatchEnd` around them, so `EngineSync` ran a full consumer
stop/reselect/restart *per mutation* instead of once. Confirmed via
`coredumpctl` + `gdb` (2026-09-23, a real crash hit live testing this
exact fix): two such restarts back to back segfaults deep inside
PipeWire/SDL3's own stream teardown (`pw_stream_destroy` →
`unref_plugin` → `dlclose`), unrelated to anything this app controls —
purely a consequence of tearing the real audio device down and rebuilding
it twice in immediate succession. Every multi-mutation command in
`core/commands/primitives.cpp` now wraps its whole apply()/revert() in
one `BatchBegin`/`BatchEnd` pair (matching the pattern `InsertClip`/
`ResizeClip`/`CompositeCommand` already used), so `EngineSync` always
coalesces to exactly one rebuild per command, however many `Model` calls
it makes internally.

`masterProducerFor()` checks `Mlt::Producer::is_valid()` right after
opening an asset's file and, on failure, substitutes a `color:black`
placeholder (sized to the asset's own recorded length) instead of caching
the broken producer — confirmed empirically (a standalone repro,
2026-09-23) that an **invalid producer still lets `.cut()` "succeed"**:
the resulting cut reports `is_valid()==true`, appends to a playlist with
no error, and the tractor built from it reports a normal length — it only
segfaults once a real frame is pulled through the live consumer, deep
inside MLT's own `mlt_producer_seek`/`transition_get_frame`. That means
open time is the *only* place this can be caught; by the time a bad
producer would otherwise reach the playlist, it's indistinguishable from
a real one. `EngineSync::mediaUnavailable` fires (main thread, from
`rebuildAll()`) so the app layer can tell the user which file is missing;
`verify()` skips its resource-match check for these clips, since the
placeholder's resource is the intended fallback, not a sync bug.

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

### Thumbnail cache notes

`ThumbnailCache` (`src/engine/thumbnail_cache.{h,cpp}`) is the same
jobs-on-the-pool-plus-cache architecture as `WaveformCache` above (doc 19
MT3: at most a capped number of jobs at once, so imports and probes keep
threads). Pending frames are batched per file: one job opens the producer
once and walks that file's frames, newest first, skipping any the view
stopped asking for. A representative thumbnail is keyed on the resource
path alone (it isn't tied to any sequence's fps the way waveform peaks
are).

**MLT caps live avformat decoders process-wide; raise the cap.** MLT keeps
at most 4 avformat producers' decoder state (`mlt_cache`,
"producer_avformat") and evicts the least recently used when another one
decodes. With cache jobs decoding on pool threads while playback decodes
its own masters, producers evicted each other mid-decode across threads.
Playback crashed in `producer_get_audio → init_cache` (reproduced
2026-09-24; 2 of 2 runs with a 1080p timeline playing while the caches
filled, 0 of 3 after). `FactoryPolicy::raiseAvformatDecoderLimit()` sizes
it as kdenlive does, threads + 2 per track. It is set once before any
other thread exists, and raised from `EngineSync::rebuildAll()`.
Each job opens its own throwaway `Mlt::Profile`/`Producer`, seeks to 10%
into the clip (a plain frame 0 often lands on a fade-in or black open),
decodes one frame, and box-downsamples it in software to a fixed
120px-wide RGBA thumbnail, converted to a `GdkTexture` the same way
`PlaybackController`'s own live-frame callback already does
(`gdk_memory_texture_new(..., GDK_MEMORY_R8G8B8A8, ...)`).

A throwaway `Mlt::Profile` defaults to MLT's own `dv_pal` (720x576,
16:15 sample aspect, 4:3 display) — an audit (2026-09-22) found that the
loader's normalising filters scale and pad every decoded frame to fit
that profile, so `get_image()` returned 720x576 with black letterbox
bars and squashed pixels for any real 16:9 source, regardless of its
actual shape (verified with a standalone repro: a rendered 1920x1080 red
clip decoded to 720x576, with the top/bottom couple of rows reading
black instead of red). Fixed by priming with one throwaway `get_frame()`
first (populating `meta.media.width`/`height`, per the lazy-population
finding above) and reconfiguring the *same* `Mlt::Profile` object's
width, height, and sample aspect (1:1) before decoding the real
thumbnail frame — confirmed empirically that the producer does not need
to be reopened for this to take effect (`tests/engine/
test_thumbnail_cache.cpp`'s own E2 test renders a real 1920x1080 clip
and checks the thumbnail comes out 120x67, matching the source's real
16:9 shape, not 120x96, dv_pal's).

`AppWindow::onThumbnailReady()` skips its `refreshMediaBrowser()` call
(destroys and recreates every row) when the panel is hidden (audit A4)
— importing N assets at once used to trigger N full rebuilds regardless
of whether the panel was even visible, and it starts collapsed by
default. `onToggleMediaBrowserClicked()` already runs its own
`refreshMediaBrowser()` when the panel goes from hidden to visible, so
opening it afterward still shows everything that finished in the
meantime, just in one rebuild instead of N.

Import also reads an asset's fps and pixel dimensions off the producer's
own `meta.media.frame_rate_num`/`_den`/`width`/`height` properties
(`EngineSync::probeMedia()`) — confirmed empirically (a standalone repro
against a rendered test file, and `tests/engine/test_probe_media.cpp`)
that these are populated **lazily**, only after the producer has actually
decoded at least one frame, not at open time; a still image never sets
them at all (`meta.media.*` is avformat-specific), so they're left at
their zero default there. The asset's recorded "format" (its container)
is read from the filename extension, not any MLT property — `meta.media.*`
has no reliable container/format string to read.

`probeMedia()` also reads whether the asset actually has audio, rather
than guessing "true whenever it isn't a still image" as it used to
(audit E3, 2026-09-22 — the old guess meant a video-only file got
waveform-decode jobs for a silent track and a "Split Audio" menu item
that produced an empty clip). Verified against the avformat producer's
own YAML metadata (`producer_avformat.yml`'s `audio_index`: "Choose the
absolute stream index of audio stream to use (-1 is off)") and a
standalone repro rendering two real MP4s, one muxed with an AAC track
and one without: `audio_index` is auto-detected and set at *open* time
(no frame decode needed first, unlike `meta.media.*` above), -1 exactly
when the container has no audio stream. A `property_exists()` guard
matters too — a non-avformat producer (a generator like `color:`) has
no `audio_index` property at all, and `get_int()` on a missing property
returns `0`, indistinguishable from "stream 0" if read unguarded
(confirmed with the same repro); `tests/engine/test_probe_media.cpp`
covers all three cases (real video with audio, real video without,
generator).

### Render implementation notes

**A render can be cancelled.** `renderProject()` starts the avformat
consumer and waits on it itself (`Consumer::run()` is just `start()` plus a
wait for "consumer-stopped"), so a cancel flag can stop it from the render
thread; the `.part` is removed. The app owns the render thread, asks before
quitting mid-render, and cancels and joins it before MLT is closed (post-M3
audit P2: quitting mid-render used to crash). A finished render reads
stopped only after avformat has written the trailer and closed the file.

`renderProject()` uses MLT's `avformat` consumer, with properties confirmed
against its actual YAML metadata rather than guessed from ffmpeg CLI-flag
muscle memory (`vcodec`/`acodec`/`f`/`vb`/`ab`/`ar`/`channels`/`pix_fmt`/
`real_time` — not, say, `b:v`/`crf`). The target format itself — h264 High
profile, yuv420p, 1920×1080, 30fps, ~923kbps video / AAC-LC 48kHz stereo
~126kbps, MP4 — was read directly off a real reference export file via
`ffprobe`, and the whole pipeline (profile choice, consumer properties) was
validated with a standalone render-then-reprobe round-trip that confirmed
an exact match before any of it was wired into the engine.

The H.264 encoder is `libx264` where ffmpeg has it, otherwise
`libopenh264` (`engine::h264Encoder()`, which asks avformat for its encoder list
once). Stock Fedora's `ffmpeg-free` ships only the latter. Given an unknown
`vcodec`, avformat logs "unrecognised - ignoring" and writes an MP4 with **no
video stream**, and `run()` still returns 0.

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

`renderProject()`'s optional `onProgress` callback (enhancement #13,
2026-09-23) hit two findings worth recording. First, which MLT consumer
event to use: `PlaybackController::handleFrameShow` (live playback)
listens for `"consumer-frame-show"`, and reaching for that same event
here seemed obvious — it got zero callbacks against a real render.
`mlt_consumer.h`'s own doc comment explains why: `"consumer-frame-show"`
is fired by *subclass* implementations only, on actually showing a
frame, and `avformat` never shows anything, it just encodes. The event
that IS fired by the *base class*, for every consumer type, before
rendering each frame, is `"consumer-frame-render"` — switching to that
fixed it, confirmed via a standalone repro (`consumer.listen()` on both
event names against a real `avformat` render: 0 "show" callbacks, 90
"render" callbacks for a 90-frame clip) before relying on it here.
Second, and much less obvious: the throttle guarding how often
`onProgress` actually fires used `std::chrono::steady_clock::time_point`
defaulted to `::min()` as a "never called yet" sentinel, reasoning that
`now - min()` would always be a huge, safely-passes-the-throttle
duration. It isn't — `min()` is near the underlying representation's
most negative value, so subtracting it from a normal "now" overflows the
duration's signed integer rep, wrapping around to a garbage (in practice,
strongly negative) result that *failed* the throttle check on every
single call. The render completed correctly and the event listener fired
every time (confirmed via temporary logging inside the trampoline before
finding this), but the callback the whole feature depends on was never
actually reached — a real doctest run against this exact code (not just
a standalone repro) is what caught it, since the repro above used
`fprintf`, not the throttle logic itself. Fixed by making the sentinel an
`std::optional<time_point>` instead: no arithmetic against a
near-out-of-range value, no overflow to go wrong.

### Settings and GSettings notes

`Settings` (`src/app/settings.h`/`.cpp`) wraps the `com.ustudio.VideoEditor`
GSettings schema for the Settings dialog's General/Playback tabs. One
sharp edge drove its whole shape: `g_settings_new(schema_id)` **aborts the
process** (`g_error()`) if the schema isn't found — fine for an installed
app with its schema as a hard dependency, wrong for this app's normal dev
loop (CLAUDE.md: launch straight from `builddir`, no `meson install`
first), where the schema is routinely *not* compiled/installed yet. Every
constructor call therefore goes through `g_settings_schema_source_lookup()`
first (returns `nullptr` instead of aborting), confirmed by a standalone
repro to not crash against a nonexistent `GSETTINGS_SCHEMA_DIR`; when it
comes back null, every getter falls back to its hardcoded default and
every setter is a silent no-op (matches CLAUDE.md's frei0r-degrade
precedent: detect and degrade at runtime, don't crash). `data/meson.build`
compiles the schema into `builddir/data/gschemas.compiled` for this
uninstalled case — point `GSETTINGS_SCHEMA_DIR` there, or use
`meson devenv -C builddir` (which sets it automatically), to exercise real
persistence during development; a real `meson install` also works, via the
system schema search path.

Testing this without polluting the real system dconf database (CLAUDE.md:
verification must never touch real user state) uses `GSETTINGS_BACKEND=memory`
— a real, documented GIO env var selecting GIO's in-process memory
backend — confirmed via a standalone repro (set a value through it, then
read the same key back through the normal `dconf`/`gsettings` CLI: the
real system value was provably untouched). `tests/app/test_settings.cpp`
runs with both env vars set (schema found, real in-memory persistence);
`tests/app/test_settings_missing_schema.cpp` runs with `GSETTINGS_SCHEMA_DIR`
*and* `XDG_DATA_DIRS` pointed at empty scratch directories (the system
schema source is derived entirely from `XDG_DATA_DIRS`, so both need
overriding to actually simulate "nothing installed") to cover the degrade
path — confirmed via the same kind of standalone repro that
`g_settings_schema_source_lookup()` genuinely returns null rather than
crashing in that state, before relying on it in the test.

### Project files

Save/load uses **MLT XML with `ustudio:`-namespaced properties** (ADR-004),
not a bespoke format — `.ustudio` files are valid MLT XML that `melt` (and, from M6,
`u-studio-render`) can play with no editor involved, because the
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

Since format version 4 (2026-09-24) the saved MLT graph is the same one
`EngineSync` plays, so `melt project.ustudio` (or MLT's `xml` producer)
plays a saved project exactly as the editor does. That includes
dissolves, per-track volume, and Split Audio's switched-off streams:
`tests/engine/test_xml_playback` compares every frame's picture and sound
against live playback. Each track is written twice:

- a **render playlist** the sequence tractor plays. It uses the same
  segments as `EngineSync` (`core::planTrackSegments()`, shared), a nested
  two-track sub-tractor per dissolve, a variant producer for each clip with
  a stream switched off, and the track's `volume` filter;
- a **record playlist** with the model's clips and their `ustudio:*`
  properties. The tractor never references it, so MLT loads it standalone
  and ignores it (like `main_bin`), and it's what the reader reads.

Two MLT loader findings drove the details, both confirmed with standalone
repros:

- A generator shorthand (`color:red`, `tone:`) stored as a bare `resource`
  loads as black and silence. It has to be written as `mlt_service` plus
  its argument; the original path is kept in `ustudio:path`.
- Stream switches only work on a producer, never on a cut (see "Engine sync
  notes").

Format 3 files (everything saved before) still open: they differ only in
where the render structure and dissolve metadata live.

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

"Still alive" cross-checks the recorded pid's own process *start time*
(`/proc/<pid>/stat` field 22, field 2's parenthesized `comm` handled by
finding the last `)` on the line, not the first space, since it can
itself contain spaces or parens), not just whether some process
currently holds that pid (audit A3, 2026-09-22): a pid alone can be
reused by an unrelated process well within an orphaned autosave's
realistic lifetime, which would otherwise make the old owner look
permanently "still alive" and hide that autosave forever. A meta file
written before this field existed has `ownerStartTime == 0`, treated
permissively (same convention as `ownerPid == 0`) rather than refusing
every pre-A3 autosave.

That deferred-cleanup path is only safe as long as it's forgotten the
moment `m_model` is replaced by anything OTHER than the Recover it was
set up for. Before an audit fix (A1, 2026-09-22), New Project/Open/
Reload left it pointing at the just-recovered autosave; if the owner
then saved that DIFFERENT (new/opened/reloaded) project, `onSaveFinished
()`'s cleanup ran anyway and deleted the recovered autosave — the only
copy of the original unsaved work, silently gone the moment an unrelated
project happened to get saved next. All three now clear both pending-
cleanup paths themselves before replacing `m_model`; the autosave files
on disk are left untouched either way, so a later launch (or this one's
own `offerRecoveryIfAny()` loop above) can still find and offer them.

When several autosaves independently qualify for recovery at once (an
agent, or the owner, bouncing the app through many untitled test launches
without ever doing a Save leaves one behind per launch), `findRecoverable()`
returns the most recently written candidate rather than whichever one a
`directory_iterator`'s unspecified order happened to yield first — the
latter was confirmed to reproduce a real report of "recovered project only
had one track and no edits" when a richer autosave existed alongside older,
thinner ones. `AppWindow::offerRecoveryIfAny()` also now loops: recovering
(or discarding) one candidate immediately checks for another, so every
independently orphaned autosave gets its own dialog in the same launch
instead of older ones going silently unmentioned once a newer one has been
handled. Recovering never deletes the file it read (see above), so the
exclusion is by an in-memory set of already-offered `.meta` paths, not by
removing anything from disk.

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
