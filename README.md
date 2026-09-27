# u Studio Video Editor

## Install the beta

The latest Flatpak is at
<https://software.unicornviz.com/u-studio-video-editor-latest.flatpak>. It
runs on any distro with Flatpak (tested on Fedora 44; made for Linux Mint
22).

```sh
# one-time: Flathub provides the GNOME runtime the app needs
flatpak remote-add --if-not-exists --user flathub https://dl.flathub.org/repo/flathub.flatpakrepo
# download and install (the first install also fetches the ~450 MB runtime)
wget https://software.unicornviz.com/u-studio-video-editor-latest.flatpak
flatpak install --user ./u-studio-video-editor-latest.flatpak
# run
flatpak run com.ustudio.VideoEditor
```

- **Linux Mint:** you can also double-click the file to open it in Software
  Manager.
- **Update:** download the new file and install it again. If it says
  "already installed", you have that version; add `--reinstall` to install
  it anyway.
- **Uninstall:** `flatpak uninstall --user com.ustudio.VideoEditor`.
- **Logs** are in
  `~/.var/app/com.ustudio.VideoEditor/.local/state/ustudio/logs/`.
  Help › About › Copy Diagnostics puts the details for a bug report on the
  clipboard.

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
  a row with a thumbnail, name, length, fps, and format, and badges under
  the name (0.50.0): what it is (IMAGE, SEQUENCE, AUDIO), its resolution
  (4K, 1440p, 1080p, 720p, SD, or its size; cyan when taller than the
  project, where a proxy would help), and its state (PROBING, FAILED,
  MISSING, PROXY, PROXY n%, PROXY MISSING). The list is a GtkListView that
  rebinds only the rows that changed, so a 500-asset bin refreshes in
  about 1.5 ms instead of rebuilding every row (80-105 ms); filling it the
  first time it's shown costs about 55 ms, since a list view keeps ~200
  rows ready whatever their height. Thumbnails are
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
  is skipped without stopping the rest. Import Folder (`Ctrl+Shift+I`, or
  drop a folder) brings in every file under it, subfolders included and
  hidden files skipped, into the bin. However many files, one import is one
  undo step, and when some can't be imported a dialog lists each one and why
  (empty, a folder, not media, or over 20 s to open). A project whose
  media has moved opens anyway: those clips show striped red (and play dark
  red), a banner says how many files are missing, and Relink… points each at
  its new place, one file at a time or by searching a folder, as one undo
  step that changes nothing else. A render with missing media asks first.
  Proxies: right-click a clip in the media browser for Create Proxy (540p by
  default, Settings › Performance) or Create Conformed Proxy (full size at
  the project's rate, for phone and screen recordings); footage taller than
  1080p is offered them once per project. They're rendered by
  `u-studio-render --proxy` in the background, into the user cache, and the
  Proxies toggle beside the preview scale plays them or the originals.
  Renders always use the originals.
  Both Import and Open Project filter their file
  pickers to media files and `.ustudio` projects respectively.
- Place pictures on the preview, OBS-style (0.48.0): click a picture to
  select its clip, drag it to move, corner handles to scale (Shift frees
  the aspect), edge handles to stretch, the knob to rotate (Shift: 15°
  steps), Alt with an edge or corner to crop. It snaps to the frame and to
  other pictures (Ctrl places freely); arrow keys nudge after clicking the
  preview. Right-click for Reset, Fit, Stretch, Centre, Flip and Rotate;
  Edit Transform (Ctrl+T, or double-click) sets exact numbers in a window
  that stays open beside the preview. Each drag or dialog session is one
  undo step.
- Multi-track timeline: add/remove tracks, drag a track's handle to reorder
  it, click a row to make it the active track (where imports/splits land).
  Higher tracks composite over lower ones for video (top wins; each
  picture fits the frame, centred, unless moved, scaled, rotated, cropped
  or flipped by its clip transform, saved with the project);
  all tracks mix together for audio, including tracks that are audio-only.
  A timecode ruler runs along the top, ticking every 1/2/5/10/15/30
  seconds or whole minutes/hours — whichever keeps ticks at least ~60px
  apart at the current zoom (enhancement #12, 2026-09-23).
  Zoom with Ctrl+mouse wheel (the frame under the pointer stays put) or
  `+`/`-`, and `0` to fit the whole project (also the zoom buttons at the
  left of the header bar's right-hand side); scroll sideways with
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
  end, or the timeline's own start/end) on any track, or to a marker,
  and `Shift+A`/
  `Shift+F` to the previous/next cut on the active track only. `S`/`D` move
  which track is active up/down, the same target a plain click sets.
  `Shift+S`/`Shift+D` move a single selected clip to the nearest track
  above/below where it fits at the same position (same kind of track;
  tracks where it would overlap a clip, or locked ones, are skipped; a
  clip in a dissolve is refused), or with nothing selected jump the
  active track to the top/bottom.
- Sync two clips by their sound: select two clips that overlap, or
  nearly (within 5 seconds), right-click the one that should stay put
  and choose "Sync Tracks (Audio)". The other clip moves so its sound
  lines up with the first's, frame-accurate, as one undo step. It's for
  cutting between several recordings of the same event. The match runs in
  the background (`core/audio/align`, `engine/audio_sync`). A weak or
  ambiguous match, a clip without sound, or a move that would land on
  another clip is reported and nothing moves.
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
- The header title shows the project's size and rate ("1920×1080 · 30
  fps"); click it to change the project's frame rate. Every clip, dissolve,
  fade, keyframe and marker keeps its time (positions move to the nearest
  frame at the new rate), as one undoable step.
- Mixed frame rates: clips of any rate (23.976 to 60 fps) play, seek and
  render on one timeline; MLT takes each source's frame nearest in time.
  A new, empty project takes its size and rate from the first video
  imported into it (one undo step with the import); a later import at
  another rate says so in the import summary ("frames will repeat" or "be
  skipped"). The media browser shows rates as 23.976, 29.97, 59.94.
- Render the project to an MP4: H.264
  (libx264, or libopenh264 where ffmpeg lacks it), yuv420p, AAC 48kHz
  stereo, via the header bar's "Render…" button, with the default render
  profile. Profiles (Settings > Render) set the output height (Project,
  2160p, 1440p, 1080p, 720p; the width follows the project's shape) and
  the quality: Draft/Good/High/Max are x264 CRF 28/23/18/14 with presets
  veryfast/medium/slow/slower (bitrates scaled to the picture size on
  OpenH264, which has no CRF), or exact bitrates. Built-ins: "High
  quality" (the default) and "Draft (legacy)", the fixed bitrates every
  render used before 0.36; duplicate one to make your own, kept in
  `$XDG_CONFIG_HOME/ustudio/render-profiles.ini`. A global Render threads setting
  (Settings > Render, default 80% of the hardware threads, warns above
  80%) splits the render between MLT's parallel frame rendering and the
  encoder; it applies from the next render. A profile's frame rate
  (Project, 23.976, 24, 25, 29.97, 30, 50, 59.94, 60) other than the
  project's renders a retimed copy of the project (`core::retime()`:
  absolute positions rounded, so every cut stays within half a frame of
  its time; doc 12, "Frame rate"). Renders run one at a time from a queue, each fixed when
  queued (the project as it was, the profile, the output path). The Render
  button fills magenta as it goes and shows a badge counting the queue;
  when it's done it turns cyan ("Open Render") and opens the file.
  Clicking it mid-render offers Cancel Render (the partial file is
  removed) or Queue Another. Right-click it to render or queue with any
  profile, auto-named `<project>-<profile>-YYYYMMDD-HHMMSS.mp4` in the
  default export folder; the left-click dialog is prefilled with that
  name. Quitting mid-render asks first; unfinished renders are kept in
  `$XDG_STATE_HOME/ustudio/pending-renders/` and offered again (from the
  beginning) on the next launch.
- Save/load a project as MLT XML with `ustudio:` namespaced properties
  (see "Project files" below), plus autosave and crash recovery. Autosave
  fires 2 minutes (configurable) after the last edit, on focus loss, and
  whenever the oldest unsaved edit reaches that age. So a crash or
  `kill -9` loses at most 2 minutes of work even while you edit
  continuously.
  `Ctrl+S` saves straight back to the project's own file with no dialog
  once it has one (falling back to the Save As dialog for an untitled
  project); `Ctrl+Shift+S`, or right-clicking the Save button, always
  opens the Save As dialog. Before an explicit save overwrites a file, the
  previous version is copied to `.ustudio-backups/<name>-YYYYMMDD-HHMMSS.ustudio`
  beside it; the newest 5 per project are kept (autosave doesn't do this). `Ctrl+O`/
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
  Editing/Project), a Release notes tab, and an About tab with Open Log
  Folder and Copy Diagnostics (versions, Flatpak or not, the log folder and
  the last 50 log lines; never the environment). Each category is
  a section that folds away (collapsed until opened); Help comes back with
  the sections, tab and scroll position it was left with, also after a
  restart. **Release notes** are the `<releases>` of
  `data/com.ustudio.VideoEditor.metainfo.xml` (compiled into the
  GResource; the software centre shows the same notes). Only releasable
  builds get an entry there: a beta or a release, with a `<description>`;
  everyday version bumps go in `CHANGELOG.md` only. The Controls tab and every control's
  tooltip come from one table, `src/app/ui_hints.{h,cpp}`: a hint names
  its action and the shortcut is looked up in `action_registry.cpp`, so a
  tooltip can't show a stale key. Drop-ins add their own hints, actions
  (listed under their own category) and more through `ShellHost`
  (`src/app/shell_host.h`). Settings dialog (header-bar gear
  button): General (autosave delay, recent-projects list size, maximum
  shuttle speed), Toggles (reopen the last project on startup, on by
  default and skipped when there's work to recover; snap while dragging;
  follow the playhead while playing; timeline thumbnails; waveforms;
  thumbnails in clip tooltips: a clip's tooltip, after a 300 ms rest,
  shows its name, in–out, length and source, topped on a video clip by the
  source frame under the pointer, 240 px wide, with its timecode),
  Performance (default preview scale, worker threads, thumbnail and
  waveform jobs) and Locations (default project folder, where Open and
  Save As start; default export folder, where renders go, else the
  project's folder, then Videos, then home) and Drop-ins (each installed
  drop-in with an on/off switch, applied from the next start, a switched-off
  module not even opened; the ones this build knows of that aren't
  installed, with the package to install; any that couldn't load, with
  why; nothing is ever downloaded) tabs, backed by
  real `GSettings` persistence (`data/com.ustudio.VideoEditor.gschema.xml`)
  — an installed schema first; otherwise a binary run straight from
  `builddir` uses the build's own compiled schema (`builddir/data`, found
  relative to the executable), so settings persist without `meson install`
  or `GSETTINGS_SCHEMA_DIR`. With no schema at all it falls back to
  in-memory defaults, with a banner in the window and a toast in Settings. A placeholder Keyboard
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

Beta testers: install the Flatpak bundle instead. How to build and
install it is in [`packaging/flatpak/README.md`](packaging/flatpak/README.md).

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
- `src/render/` — `u-studio-render`: runs drop-ins' subcommands
  (`--help` lists them; `render_cli.h`); the real headless render CLI is
  milestone M6.
- `tests/core/`, `tests/engine/`, `tests/app/` — doctest suites (vendored under
  `subprojects/doctest/`, no system package needed). `tests/engine/`
  needs MLT but no display and no media files.
### Implementation notes

The empirical MLT, GTK and GLib findings that used to follow here now live
in [`docs/developer/notes/`](docs/developer/notes/README.md), one file per
area: playback engine, engine sync, media caches, render, settings,
project files, and the app shell.
