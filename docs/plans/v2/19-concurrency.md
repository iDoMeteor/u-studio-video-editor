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
    rebuild (212 ms). Getting under 16 ms needs MT2.
- Save and autosave serialise a snapshot on the pool (the atomic temp file
  and rename stay); the dirty flag is cleared only when the write that
  matches the current undo depth succeeds.
- Project load parses on the pool; the model swap happens on the main
  thread.

### MT2 — Engine thread (about 2 weeks; the core of this plan)

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

- [ ] During each of these, no main-loop iteration exceeds 16 ms (stall
      monitor): importing 50 files; saving a 5,000-clip project; 100 edits
      in 10 s while playing 4K.
- [ ] Edits while playing never block input; playback picks up the newest
      graph within one rebuild.
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
