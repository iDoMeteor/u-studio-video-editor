# ADR-016: The main thread only does UI and commands; an engine thread, a worker pool and child processes do the rest

**Status:** Accepted (owner, 2026-09-24: proceed with the multi-threading
plan, starting with MT0; proposed the same day on owner direction to make
the editor multi-threaded before going much further). The doc 02 and
CLAUDE.md threading-rule changes land with MT2, when the engine thread
exists. Design in
[doc 19](../19-concurrency.md).

## Context
Doc 02 already required the main thread never to block for more than
5 ms, but the code rebuilds the MLT graph, restarts playback, probes
media and saves projects on the main thread (30–80 ms per edit measured,
more for large projects), and export runs on a single render thread in
the editor's own process. The owner wants the editor to use the hardware's
parallelism throughout.

## Decision
- The GTK main thread owns widgets, the `Model` and the `UndoStack`, and
  does input, commands and drawing only.
- A single **engine thread** owns `EngineSync`, the master producers, the
  live tractor and `PlaybackController`. The main thread sends it
  commands and model snapshots; it never shares MLT objects with any other
  of our threads.
- A **worker pool** (std-only, priorities, `std::stop_token`
  cancellation, sized to about half the hardware threads by default) runs
  probing, caches, save/load serialisation and analysis. Each job owns the
  MLT objects it creates.
- Export and proxies run in **child processes** (ADR-009).
- Threads other than main read **immutable model snapshots**
  (`std::shared_ptr<const Project>`) and return values through
  `MainThreadDispatcher`; model changes happen only through commands on
  the main thread. Pending work is latest-wins.
- No thread ever blocks waiting on the main thread.

## Consequences
- Doc 02's threading table and CLAUDE.md's threading rules change
  (EngineSync and PlaybackController leave the main thread) once this is
  accepted.
- Playback reflects an edit a moment after the model does, instead of
  synchronously.
- Tests must wait on conditions, not sleeps; TSan becomes routine.
- Drop-in hooks get a documented thread each (doc 15, IP3/IP5/IP6).
