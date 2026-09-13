# 00 — Review of the v1 milestone

Scope of the inspection: every file in `src/`, the root `README.md` (the only
planning document that existed), `meson.build`, the `builddir` state, and the
libraries actually installed. Plus two throwaway probes compiled against the
installed MLT to check facts the v2 design leans on. Nothing in `src/` was
modified by this review.

**Snapshot note.** The first read was of the 20:47 build (7 files, 889
lines). The team kept working during the review; by 21:32 `src/` had gained
`util/log.{h,cpp}`, a wall-clock playback schedule in
`MltEngine::pullLoopMain()`, an atomic `m_totalFramesCache`, and multi-track
scaffolding in `mlt_engine.h` (`m_tracks`, `addTrack()`; the `.cpp` was still
mid-edit). This doc refers to code by **function name**, not line number,
for that reason, and the "in-flight" paragraphs below fold those changes in.

## What v1 does well (keep these)

- **The MLT boundary is real.** `mlt_engine.h` forward-declares
  `Mlt::Profile/Playlist/Tractor` and `pa_simple`; no MLT or Pulse header
  leaks above the engine. v2 keeps this rule and adds a second one of the
  same shape for GTK (see [02-architecture.md](02-architecture.md)).
- **Main-thread marshaling is explicit.** Frames cross to GTK through the
  GLib main loop (`g_idle_add` → `MltEngine::deliverOnMainThread`), never by
  calling widgets from the worker. Right idea; v2 hardens it.
- **Split success is detected empirically** in `MltEngine::splitAt()`
  because `Playlist::split_at` has no status return. That "don't trust the
  return value, check the state" instinct is exactly what the v2 engine
  consistency checker generalises.
- **Empirical MLT findings are written down** next to the code: the
  `get_frame()` auto-advance discovery in `pullLoopMain()` (explicit
  `seek(pos+1)` per frame turns sequential decode into random access and
  wrecks audio on long-GOP files) is precisely the kind of MLT knowledge v2
  wants captured in `engine/` comments and tests.
- **libadwaita named-color remapping** (`style_css.h`) reskins the whole shell
  from one CSS block. v2 keeps this and moves it into a GResource.
- **In flight: `util/log.h`.** A small, thread-safe, dependency-free logger
  with an env-var level and a per-run file. v2 keeps this API as the logging
  front door (doc 14) and moves it into `core/` in M0 since it has no deps.
- **In flight: `m_totalFramesCache`.** Answering `totalFrames()`/`seek()`
  without taking the decode mutex is the right instinct and removes the
  worst of P1's UI stalls.

## Problems that v2 must fix

Ordered by how much they hurt as the app grows.

### P1. Timeline state lives inside MLT

`AppWindow::m_clips` is a cache of `MltEngine::clips()`, which walks the
`Mlt::Playlist` under `m_mltMutex`, the same mutex held for the whole decode
of a frame inside `pullLoopMain()`. The README already flags the coupling.
Consequences: no undo, no save/load without round-tripping MLT, no way to
test an edit without a media file, and `clips()` from a redraw can still
stall behind a 30 ms decode (the frames cache fixes `totalFrames()` only).

**v2:** a pure-C++ project model is the single source of truth; MLT is a
projection of it ([03-project-model.md](03-project-model.md), ADR-003).

### P2. Playback is a hand-rolled clock

The original loop paced itself by blocking on `pa_simple_write()` with a
~150 ms server buffer. The team's in-flight rewrite of `pullLoopMain()`
documents exactly why that fails (an underrun makes the next write return
instantly and the loop races ahead) and replaces it with a wall-clock
schedule anchored at play/seek. That is a real improvement, but the
remaining structure still has three problems no amount of tuning fixes:

1. **Audio still leads/lags video by the Pulse buffer depth.** The video
   frame is posted to the UI before the audio for the same frame is even
   written, and the audio then sits in a ~150 ms buffer. The offset is
   roughly constant, so it's easy to miss on speech and obvious on cuts to
   music.
2. **No frame dropping.** When decode can't keep real time, the loop stops
   sleeping but still decodes every frame, so playback goes slow-motion
   while audio writes block and underrun (the "clicks" the comments
   mention). MLT consumers drop video frames and keep audio continuous.
3. **No speed, reverse, or loop**, and every one of those would be another
   hand-written clock path.

**v2:** use MLT's own `sdl2_audio` consumer (present on this system; see
"Verified facts"), which owns the clock, does A/V sync, real-time frame
dropping, speed/reverse, and preview scaling. Video reaches GTK via the
`consumer-frame-show` event, the pattern kdenlive uses
(`~/Repos/kdenlive/src/monitor/videowidget.cpp`, `VideoWidget::reconfigure`
and `on_frame_show`). See [05-playback-engine.md](05-playback-engine.md),
ADR-002.

### P3. Frame delivery copies twice and can pile up

Each frame is copied out of MLT into a vector (`rgba.assign(...)` in
`pullLoopMain()`) and copied again by `g_bytes_new` in
`AppWindow::onFrameReady()`. At 1080p25 that is ~415 MB/s of memcpy for the
copies alone. Separately, `g_idle_add` is called per frame with no
back-pressure: if the main loop stalls, idles queue without bound and the UI
replays a burst of stale frames afterwards. The team's own comment in
`pullLoopMain()` records a case where this "flooded `g_idle_add()` fast
enough to make the whole app unresponsive".

**v2:** hand the buffer to GTK with `g_bytes_new_with_free_func` (one copy,
no second), and coalesce to "latest frame wins" with a single pending slot.

### P4. Use-after-free hazard on shutdown

`PendingFrame` carries a raw `MltEngine*`. If the engine is destroyed while
an idle is queued, `deliverOnMainThread` dereferences freed memory. It is
masked today only because `AppWindow` (and thus the engine) is intentionally
leaked in `main.cpp`'s `onActivate`. v2 has multiple documents and can't lean
on that.

**v2:** a `MainThreadDispatcher` with a lifetime token
([02-architecture.md § Threading](02-architecture.md#threading-model)).

### P5. Fixed profile, weak import validation

The profile is hardcoded to `atsc_1080p_25` in the `MltEngine` constructor;
any 30/60 fps source is resampled to 25. `importClip()` only checks
`is_valid()`; MLT's loader will happily return a "valid" producer for a
directory or an unreadable file, with length 0 or 1.

**v2:** project profile chosen at creation or inferred from first import;
probing runs on a worker and checks length, streams, and `mlt_service`
([07-media-bin-and-assets.md](07-media-bin-and-assets.md)).

### P6. No tests, no CI, no packaging metadata

There is no `tests/` directory, no desktop file, no AppStream metainfo, no
icon, no GResource, no `.clang-format`. Everything is verified by running the
binary and reading `logs/`.

**v2:** [11-build-test-ci-packaging.md](11-build-test-ci-packaging.md).

### P7. The "no Qt in the stack" claim is currently false at runtime

The README's `ldd | grep -i qt` check passes, but it only inspects the link
line. `Mlt::Factory::init()` with no directory argument (as called in the
`MltEngine` constructor) dlopens every module under `/usr/lib64/mlt-7/`,
including `libmltqt6.so` and `libmltglaxnimate-qt6.so`, which pull Qt6 into
the process. Measured: **32 Qt library mappings** in `/proc/self/maps` after
`Factory::init()`.

**v2:** initialise the factory with a curated module directory (verified to
give 0 Qt mappings, see below). ADR-007.

### P8 (in flight). Multi-track is being added to the MLT-owned model

`mlt_engine.h` now declares `std::vector<std::unique_ptr<Mlt::Playlist>>
m_tracks` and `addTrack()`. Adding tracks *inside* the MLT-owned state
deepens P1: every new operation (move between tracks, overlap checks,
compositing transitions) has to be written against MLT playlists and then
re-derived for display. It's fine as a spike to learn the MLT track/transition
API, and that knowledge feeds doc 05 and doc 08 directly. The recommendation
is to treat it as a spike, not as the v2 track model, and to land M1 (the
core model) before building UI on it.

## Smaller things noted

- `MltEngine::fps()` reads `m_profile` without the mutex. Safe today because
  the profile is immutable after construction; v2 makes profile immutability
  explicit.
- `AppWindow::onTimelineClicked` maps x to a frame assuming the whole timeline
  fits the widget width. There is no zoom/scroll concept; v2 introduces a
  viewport ([06-timeline-ui.md](06-timeline-ui.md)).
- Selection (`m_selectedClip`) is drawn but nothing acts on it.
- `formatTimecode` rounds fps to an integer; 29.97 sources will show drifting
  timecode. v2 uses rational fps and proper NDF/DF handling (initially NDF).
- Clip colours are duplicated by hand between `style_css.h` and the
  `kClip*` constants in `app_window.cpp`; v2 generates both from one token
  source.
- `logs/` is written relative to the current working directory
  (`Log::init`). Fine for development; v2 moves it to `$XDG_STATE_HOME`.
- `meson.build` lists `libpulse-simple`, which v2 drops in favour of MLT's
  consumer (which uses SDL2 → PipeWire/Pulse).
- Fonts: still not bundled. v2 ships them in the Flatpak; on bare metal keep
  the CSS fallbacks.

## Verified facts (probes, 2026-09-12)

These were established by compiling two small programs against the installed
MLT, not by reading docs. Re-run them if the machine or MLT version changes;
they become a real test in M0 (`tests/engine/test_factory_policy.cpp`).

| Fact | Result |
|------|--------|
| Consumers available with default `Factory::init()` | `decklink blipflash cbrts xml avformat xgl multi null rtaudio sdl2 sdl2_audio qglsl jack` |
| Consumers with curated dir (all modules except `*qt6*`) | `xml sdl2 sdl2_audio rtaudio blipflash cbrts xgl jack decklink multi null avformat` — everything we need remains |
| Qt libs mapped, default init | 32 |
| Qt libs mapped, curated dir | **0** |
| Profiles still resolve with curated dir (`atsc_1080p_25`, `hdv_720_25p`) | yes; `MLT_DATA=/usr/share/mlt-7` |
| Transitions available (curated) | `affine movit.luma_mix movit.mix movit.overlay composite luma mix matte` |
| `frei0r-plugins` installed | **no** → `frei0r.cairoblend` (kdenlive's default compositor) is unavailable; use `composite`/`affine` (ADR-006) |
| `qtblend`, `qtext` | Qt-only, excluded by policy |
| Text rendering without Qt | `pango` producer and `dynamictext` filter present (gdk / plus modules) |
| Filters | 526 total, of which 333 are `avfilter.*` |
| SDL2 | provided by `sdl2-compat` 2.32 (SDL3 underneath); `sdl2_audio` consumer loads |
| Unit-test frameworks installed | none (`catch`, `gtest`, `doctest` all absent) → vendor doctest, see doc 11 |
| Sibling reference | `~/Repos/kdenlive` is a current checkout (commit 4234e85, 2026-09-12); useful for MLT usage patterns, never for code copying (GPL is compatible, but the point of this project is a different architecture) |
