# 19 — Concurrency: making the editor multi-threaded

**Status:** proposal, 2026-09-24. Owner direction the same day: "we want
to be as multi-threading compatible as possible … we need to make the
whole thing multi-threaded before we get much, if any, further." Decision
record: [ADR-016](adr/016-concurrency-model.md).

GTK widgets can only be touched from the main thread, so "multi-threaded"
here means: **the main thread does UI and applies commands, and nothing
else**; every other kind of work runs elsewhere, in parallel where the
hardware allows, with results handed back. Doc 02's threading model
already states the rule ("must never block > 5 ms: no sync IO, no
probing"); the code does not follow it yet, and doc 02 did not plan for
graph building and playback control to leave the main thread. This doc
closes both gaps.

## Where the time goes today

| Work | Thread today | Cost observed | Problem |
|---|---|---|---|
| `EngineSync::rebuildAll()` after every edit | main | 30–80 ms per edit (logs, 2026-09-20; one 4K asset) | blocks UI on every edit; grows with project size |
| `PlaybackController::setTractor()` (consumer stop and restart) after every edit | main | 20–40 ms | blocks UI; `stop()` joins MLT threads |
| Media probing on import (`probeMedia`) | main | one decoded frame per file, serially | multi-file import freezes the window |
| Save, autosave, project load (`saveProject`, `loadProject`) | main | proportional to project size, plus `fsync` | UI stalls, worst at autosave time |
| Frame hand-off (`handleFrameShow`) | MLT consumer | one full-frame copy per frame (8 MB at 1080p, 33 MB at 4K) | memory bandwidth on the playback thread |
| Waveforms, thumbnails | one worker each | serial queues | slow to fill a big bin or long timeline |
| Export (`renderProject`) | one detached in-process thread; MLT `real_time=-1` (one render thread) | real encode time | single render thread; crashes take the editor down (ADR-009 wants a child process) |
| Decoding | FFmpeg, automatic threads | about one thread per CPU | already parallel (measured, doc 05) |
| Playback rendering | MLT consumer, `real_time=1` (one render thread) | 4K60 below real time (doc 05) | `real_time>1` measured broken with `sdl2_audio` |

## The model

Four places work can run, each with a fixed owner:

| Thread | Owns | Does |
|---|---|---|
| **Main (GTK)** | widgets, `Model`, `UndoStack` | input, applying commands (fast, in memory), drawing, receiving results. Never blocks for more than a few milliseconds. |
| **Engine thread** (one, new) | `EngineSync`, master producers, the live tractor, `PlaybackController` | builds MLT graphs from model snapshots, swaps them into playback, runs play/pause/seek. Serialises all MLT graph work, so no MLT object is ever touched from two of our threads. |
| **Worker pool** (N threads, new) | nothing long-lived; each job owns its own MLT objects | probing, thumbnails, waveforms, save/autosave serialisation, project parsing, analysis jobs for drop-ins |
| **Child processes** | everything in their process | export and proxies (ADR-009), heavy drop-in analysis (IP6) |

MLT's own threads (consumer, read-ahead, FFmpeg decode) stay as they are;
we only size them.

### Snapshots, not sharing

The model is written only on the main thread. After each command (or
batch), the main thread publishes an **immutable snapshot**:
`std::shared_ptr<const Project>`. The engine thread and workers read only
snapshots, never the live `Model`, so no locks are needed on model data.

- Cost: a copy of `Project` per edit. Measure it first (MT0); a project
  with thousands of clips is still small next to a single video frame. If
  it ever matters, move to structural sharing (unchanged tracks shared
  between snapshots), not locks.
- **Measured (MT0, 2026-09-24):** `Model::snapshot()` copies a generated
  project of 5,000 clips on 8 tracks, with 1,992 dissolves and 200 markers,
  in a median 0.55–0.62 ms (p90 about 0.9 ms, worst 1.4 ms over 101 runs;
  plain debug build on the i7-1260P, load about 5). A cached call, with no
  edit since the last one, costs about 0.03 µs. Well under the 5 ms line,
  so no structural sharing for now. Benchmark: `tests/core/bench_snapshot.
  cpp`, run with `meson test -C builddir --benchmark core-snapshot`.
- Results come back as values (a built tractor, probed `MediaInfo`, a
  rendered thumbnail) through `MainThreadDispatcher`, and become model
  changes only through commands on the main thread.

### Latest wins

Edits can arrive faster than graphs build. The engine thread keeps only the
newest pending snapshot: while building for snapshot 7, snapshots 8, 9 and
10 collapse into "build 10 next". The UI never waits; playback shows the
newest finished graph a moment later. The same rule applies to thumbnails
for a scrolled-away row (cancelled) and to autosave (only the latest state
is written).

### Cancellation and lifetime

Every job gets a `std::stop_token`. Closing a project, removing a clip or
quitting requests stop; jobs check at natural points (between frames,
before writing). Results posted after their owner is gone are dropped by
the dispatcher's lifetime tokens (already in place).

### Sizing

The dev machine is an i7-1260P: 4 performance and 8 efficiency cores, 16
threads, shared with other work. Measured on 2026-09-24: more threads is
not automatically faster (extra decoder threads made no difference; two
consumer render threads collapsed throughput). So:

- the worker pool defaults to `hardware_concurrency / 2` (at least 2),
  configurable in Settings;
- jobs have a priority (interactive: probing what the user just
  imported, visible thumbnails; background: waveforms for off-screen
  clips, autosave);
- every new use of parallelism ships with a before/after measurement, like
  the soak and the doc 05 table.

## Workstreams

Each lands as small reviewed commits with tests, in this order.

### MT0 — Infrastructure (about 1 week)

- `core::concurrency`: a std-only thread pool with priorities,
  `std::stop_token` cancellation and a job handle; no GLib (core rules).
- Model snapshots: `Model::snapshot()` after each command or batch, with a
  benchmark on a generated 5,000-clip project.
- A **main-thread stall monitor** in debug builds: logs any main-loop
  iteration over 16 ms with what was running, so regressions show up in
  everyday use, not just in tests. Built as `app/stall_monitor.*`
  plus `core/trace.h` markers (actions, `UndoStack`, `EngineSync::
  rebuildAll`, `PlaybackController::setTractor`, the timeline snapshot).
  First reading (2026-09-24, nudges while playing, the audit's test
  project): every edit blocked the main loop 108–160 ms, all of it in
  `execute: Move clips > EngineSync::rebuildAll`, which is MT2's target.
- `just tsan` becomes part of the pre-commit routine for anything touching
  threads (it exists and passes today).

### MT1 — Import, probing, save and load off the main thread (about 1 week)

- Probing runs on the pool, in parallel across files, with progress in the
  status bar; each result becomes `AddAsset` + `InsertClip` on the main
  thread. Landed in 0.24.0 as `app/import_queue.*`, shared by Import, the
  timeline drop and the media-browser drop. Findings:
  - MLT initialises some module state lazily and without a lock (the
    loader's dictionary and normalizers, avformat's one-time init), so the
    first producers created on two threads at once can crash
    (`attach_normalizers`, about 1 run in 7 of `app-import-queue`).
    `FactoryPolicy` now walks those paths once, right after
    `Factory::init`; 150 of 150 runs clean afterwards.
  - Probing 12 files in parallel takes about 130 ms, but each apply is a
    command whose `rebuildAll` costs 110–210 ms with a dozen real clips.
    Applying every ready result in one go blocked the main loop 2.4 s, so
    results apply one per main-loop iteration: the worst stall is now one
    rebuild (212 ms). Getting under 16 ms needs MT2, so the 50-file
    import criterion below is MT2's acceptance, not MT1's (Strategist,
    2026-09-24).
- Save and autosave serialise a snapshot on the pool (the atomic temp file
  and rename stay); the dirty flag is cleared only when the write that
  matches the current undo depth succeeds. Landed in 0.25.0 as
  `app/save_queue.*`:
  - Writes run one at a time, so two writes to one path never share its
    `.tmp`. Latest wins: at most one Save and one Autosave wait behind the
    running write.
  - `UndoStack` now names states rather than depths. Each entry has a
    serial, renewed on merge; a save captures `state()` on submit and
    passes it to `setCleanPoint(State)` on completion. This also fixed a
    false "clean" after undoing below the save point and editing again.
  - Closing waits for a running write; `prepareForShutdown()` runs
    `SaveQueue::finish()`. Model `check()` runs on the pool as well.
  - Measured on a 5,000-clip project (1,992 dissolves, 200 markers; 4.9 MB
    file), three runs each, stall monitor on:
    - before: Save blocked the main loop 172–184 ms;
    - after: no main-loop iteration over 16 ms from Save onwards, and the
      write lands 187–197 ms later on the pool.
- Project load parses on the pool; the model swap happens on the main
  thread. Landed in 0.26.0 as `app/project_loader.*`:
  - Open, Reload and Recover all go through `loadProjectAsync()`. A newer
    load, or New Project, cancels the one in flight and drops its result.
  - If the user edits while a load parses, the swap asks "Discard unsaved
    changes?" again.
  - On the 5,000-clip project the parse (264–285 ms) left the main thread.
    The swap's iteration is now `rebuildAll` (2.0–2.2 s) plus 10–15 ms;
    that rebuild is MT2's.
- The pool's size is the `worker-threads` setting (0.27.0), shown in
  Settings as "Worker threads": 0, the default, is "Automatic (N)" with
  N = `hardware_concurrency / 2`, at least 2. It's read once at startup.

**MT1 status (2026-09-24): done.** Import, save, autosave and load are all
off the main thread; the timings for each are in its entry above. The
cost left on the main thread is `EngineSync::rebuildAll` after each edit
or swap: 110–210 ms with a dozen real clips, and 2.0–2.2 s with 5,000.
That is MT2's starting point, and it carries the 50-file import criterion.

### MT2 — Engine thread (about 2 weeks; the core of this plan)

**Piece 0, measured first (2026-09-24).** `rebuildAll()` scaled worse than
linearly, for two reasons, both in MLT and both fixed in 0.27.1 before any
thread work:

- **Playlist appends were O(n²) per track.** MLT refreshes the whole
  playlist after every append. Tracks with more than 64 entries are now
  built from nested sub-playlists of 64 (README, "Engine sync notes").
- **glibc's dynamic mmap threshold moved every 9.2 MB `mix` transition onto
  the heap** after the first rebuild, where `calloc()` zeroes it all. That
  meant 5.7 s spikes and resident memory reaching 36 GB at 5,000 clips.
  `FactoryPolicy` pins the threshold at 4 MiB.

Setup: `rebuildAll()` only, no consumer attached, with 40 real 300-frame
H.264 sources, a dissolve every other butt and a gap every fifth clip.
`setTractor()` restarts separately measured 128–167 ms, flat from 1,000 to
5,000 clips. Medians of 9 in a release build, before → after:

| Project | Before | After |
|---|---|---|
| 500 clips, 8 tracks | 28 ms | 25 ms |
| 1,000 clips, 8 tracks | 70 ms | 53 ms |
| 5,000 clips, 8 tracks | 1.07–2.8 s, spikes to 5.7 s | 305 ms |
| 1,000 clips, 1 track | 403 ms | 61 ms |
| 5,000 clips, 1 track | 17.3–21.6 s | 352 ms |

Chunk-size sweep (release; debug was within about 15%):

| Chunk | 1 track, 5,000 clips | 8 tracks, 5,000 clips |
|---|---|---|
| 32 | 357 ms | 288 ms |
| 64 | 352 ms | 305 ms |
| 128 | 373 ms | 306 ms |
| 256 | 441 ms | 447 ms |

32 and 64 tie. 64 keeps half as many nested playlists.

Checks on 0.27.1:
- **M2 soak** (debug build, 10 min, 4K60 H.264, 100 × 6 s clips so the
  track is chunked, preview Half): held real time. 37.2 frames/s shown
  (22,301), 22,293 delivered; resident memory 727–771 MB over the run and
  853 MB in the end report; load 3.4–4.8. The last recorded soak showed
  about 37 frames/s and 0.81 GB, so the 4 MiB mmap threshold costs no
  measurable playback throughput.
- **Stress**, 16 in parallel:
  - `engine-playback-controller`: 32 of 32 passed.
  - `engine-chunked-playlist`: 47 of 48 and then 48 of 48. The one
    failure missed a loop wrap inside the 5 s window, the same window the
    flat loop test uses, and didn't reproduce. Per-track
incremental rebuild stays shelved unless the MT2 latency targets miss.

**Piece 1: EngineSync builds from snapshots.** It holds the published
`shared_ptr<const Project>` and a private `Model` over a copy of it; it no
longer subscribes to the live Model.
- **Publishing:** the window publishes `Model::snapshot()` from
  `UndoStack::changed`, once per command, undo or redo, so a batch is one
  snapshot. `setProject()` skips the rebuild when nothing the graph reads
  changed (markers, the id allocator, settings), and ignores the same
  snapshot twice, e.g. a save marking the stack clean.
- **Swaps:** `reset(snapshot)` runs before `UndoStack::clear()`, so a
  project swap is one rebuild. `renderProject()` builds from its model's
  snapshot the same way.

**Pieces 2–4: the engine thread (0.28.0, fixes in 0.28.1).**
`engine::Engine` owns one `std::jthread` holding EngineSync and
PlaybackController (README, "Playback engine notes"). Measured on
2026-09-24; stall-monitor numbers are from the real Wayland desktop
(hardware GL). Under Xvfb, software painting of each 1080p frame alone
stalls the main loop 20–60 ms, about 27 times a second, so it can't judge
the 16 ms criterion.
- **100 edits in 10 s while playing 1080p** (120-clip project, alternating
  nudges every 100 ms): no main-loop iteration over 16 ms during the edits.
  54 builds for 100 edits (latest wins), each about 215 ms on the engine
  thread (graph plus consumer restart).
- **50-file import** (1080p H.264, window already up): no iteration over
  16 ms. That needed one app fix: imports refresh the timeline and media
  browser once per burst (`queueRefresh()`), not once per file, where the
  media browser's full rebuild had reached 17 ms by the fiftieth file.
- **Latency, publish to first frame of the new graph** (release, 8 tracks,
  median of 8): 161 ms at 500 clips (target 200: met); 525 ms at 5,000
  (472–649; target 500: **missed by about 5%**). That's roughly 300 ms of
  build, 170 ms of consumer restart and the first frame. The lever, if
  wanted, is per-track incremental rebuild or a cheaper restart.
- **Found and fixed on the way** (0.28.1):
  - Latest-wins didn't hold while playing. Every shown frame queues a drain
    on the engine thread, so snapshots are folded across drains now.
  - `play()` right behind `setTractor()`'s pause sometimes never started
    (MLT's read-ahead holds speed-0 frames that use up the sdl2 consumer's
    refresh wakes). `play()` now sets the speed, purges, then refreshes.

- `EngineSync` and `PlaybackController` move to the engine thread. The main
  thread talks to them through a small command queue (play, pause, seek,
  loop, volume, preview scale, "new snapshot") and receives state
  (position, playing, backend) back through the dispatcher.
- Graph builds use latest-wins snapshots. The consumer restart after a
  rebuild happens on the engine thread, so the UI never blocks on it.
- `setTractor()`'s existing stop-then-swap rule stays exactly as it is; it
  just runs on the engine thread.
- This changes doc 02's table (EngineSync and PlaybackController leave the
  main thread) and CLAUDE.md's threading rules; both are updated when
  ADR-016 is accepted.
- Drop-in integration point IP3 (`EngineExtension`) is defined on top of
  this: extension hooks run on the engine thread.

### MT3 — Parallel caches (about 3–5 days)

- Waveforms and thumbnails move from one dedicated thread each onto the
  pool, with priorities (visible first) and cancellation.

### MT4 — Playback throughput (about 1–2 weeks, measurement-led)

- Zero-copy hand-off: keep the `Mlt::Frame` alive and wrap its image in a
  `GBytes` whose destroy notify releases the frame, instead of copying
  every frame on the consumer thread.
- Investigate why `real_time > 1` breaks with `sdl2_audio` (5–6 frames/s,
  doc 05) with a standalone repro; if it can be made to work, expose render
  threads as a preference for 4K.
- Incremental rebuilds (ADR-005's planned optimisation: rebuild only the
  changed track) now that rebuilds are off the main thread and measurable.

### MT5 — Export in parallel, out of process (about 1 week)

- Move export into `u-studio-render` as ADR-009 requires, launched with
  `GSubprocess` and reporting progress on stdout.
- Try `real_time = -N` (parallel rendering without dropping) for export,
  checking frame-exact output against `-1` on a generated project before
  adopting it.

### MT6 — Drop-ins inherit the model

The drop-in host (doc 15, IP1–IP6) documents each hook's thread:
engine-thread hooks (IP3), main-thread UI hooks (IP5), and pool or child
process for analysis (IP6). A drop-in never creates its own long-lived
threads; it submits jobs to the pool.

## Order relative to the roadmap

1. Finish everything through M3 (in progress).
2. The post-M3 audit.
3. **MT0–MT3.** They change how every later piece is built (IP3 hooks,
   M4's import and proxies, effects parameter updates), so they go before
   the drop-in integration points and M4.
4. Drop-in integration points, then M4. MT4 and MT5 can run alongside M4.

## Acceptance

- [x] During each of these, no main-loop iteration exceeds 16 ms (stall
      monitor): importing 50 files (checked at MT2: MT1 moved the probing
      off, but each apply's `rebuildAll` stays on the main thread until
      then); saving a 5,000-clip project; 100 edits in 10 s while playing
      1080p (owner direction 2026-09-24: HD is the primary target; 4K
      throughput is MT4's, run on demand). (Save: MT1. Import and edits:
      MT2, on the Wayland desktop; see "Pieces 2–4".)
- [ ] Edits while playing never block input; playback picks up the newest
      graph within one rebuild: in a release build, from publishing the
      snapshot to the first frame of the new graph, at most 500 ms at 5,000
      clips on 8 tracks and 200 ms at 500; a burst of edits costs at most
      one build beyond the last one. (Input never blocks, the 500-clip
      target and the burst bound are met; 5,000 clips measured 525 ms.)
- [ ] The routine soak (`playback_soak`, 10 min of generated 1080p H.264,
      more than 64 clips so tracks are chunked) holds real time. The
      recorded 4K60 runs above are history.
- [ ] `just tsan` and `just asan` pass on every suite.
- [ ] Export runs in `u-studio-render`; killing the editor doesn't stop it.
- [ ] Each parallel change has a recorded before/after measurement.

## Risks

- MLT thread safety (doc 13, R1). Mitigation: one engine thread owns every
  MLT graph object; workers only use objects they created.
- Deadlocks between the engine thread and the dispatcher. Mitigation: no
  thread ever waits on the main thread; all cross-thread calls are posted,
  never blocking.
- Snapshot copy cost. Mitigation: measure in MT0; structural sharing if
  needed.
- Nondeterministic tests. Mitigation: tests wait on conditions, never
  sleeps; TSan in the routine.
