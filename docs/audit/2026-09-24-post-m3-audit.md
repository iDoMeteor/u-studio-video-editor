# Post-M3 bug audit, 2026-09-24

This covers everything that landed since the third-pass audit (`560a077`), up
to `981b70d` "Finish M3" (v0.23.0): 63 commits, 81 files and about 10,500
added lines. It is the audit roadmap step 2 asks for before M4, bugs and
sanitizers together. It is written for the owner and the code team.

Nothing in `src/` was changed. The scripts and repros are in
[`2026-09-24-post-m3-run/`](2026-09-24-post-m3-run/).

## Summary

| ID | Finding | Severity | Evidence |
|---|---|---|---|
| P1 | Removing an asset leaves its deleted clips in the timeline selection; Shift+Delete then aborts the app | High | Reproduced in the app |
| P2 | Quitting while a render runs crashes the app and leaves `<name>.part` beside the target | High | 3/3 SIGSEGV (plain build), plus an ASan SEGV |
| P3 | Stopping a just-started playback consumer can deadlock inside MLT, hanging the main thread | Medium | Plain build 5/64 under load; TSan suite timeout; not reproduced in the app |
| P4 | A no-op ripple move deletes the clip's dissolves (the ripple-mode twin of audit C3) | Medium | Core repro |
| P5 | Every zoom level queues a fresh set of thumbnail decodes, and none are ever cancelled | Medium (performance) | 102 decodes for 24 zoom steps, against 6 at rest |
| P6 | A short ripple drag inside the clip's own span previews as valid but is refused | Low | Core repro plus controller code |
| P7 | `EditMarker` merges into a no-op, and redo then asserts | Low (latent: no UI uses it yet) | Core repro |
| P8 | Undo or redo mid-drag doesn't cancel the drag | Low | By inspection |
| P9 | `just asan` is always red: a timing budget in `test_timeline_render` | Low (tooling) | ASan suite |
| P10 | Popovers are never unparented; GTK warns on every window close | Low | 7 of 7 closes |

**Sanitizers:**

- No memory-safety errors, no undefined behaviour and no data races in our
  code: in the suites, and in scripted ASan app sessions that exercised
  every M3 gesture.
- The only failures are P9 (a timing check) and P3 (the TSan run hung).
- The fuzz test, the new core commands, and format 4 all came through
  clean.

## Resolution status (2026-09-24, v0.23.1)

All ten fixed. Each has a test except P10 (checked in an app session).

| Finding | Fix | Checked by |
|---|---|---|
| P1 stale selection | The selection is pruned, and a live drag cancelled, whenever the undo stack changes (one place, `m_undoStack.changed`); ripple delete also skips ids the model lacks | `stale_selection.py` rerun: alive after Shift+Delete, Delete and nudge |
| P2 quit during render | The render thread is owned and joined; `renderProject()` takes a cancel flag (it now waits on the consumer itself, so it can stop it) and removes the `.part`; closing mid-render asks "Stop and Quit?"; `prepareForShutdown()` cancels and joins before MLT closes | `engine-render`'s new cancel test (stops within 2 s, nothing left); `render_quit.py`: render cancelled 161 ms into shutdown, exit 0, no `.part` |
| P3 consumer-stop deadlock | `prefill` = 1 on the playback consumer. From MLT 7.40's source: `mlt_consumer_rt_frame()` waits for a preroll of min(prefill, buffer) frames while the read-ahead stops at one queued frame once it reads a paused frame; with preroll > 1 both can wait, and `mlt_consumer_stop()` broadcasts once before the sdl2 consumer joins its thread. With a preroll of 1 they can't both wait | The P3 case, 16 in parallel: 2/64 hangs before, 0/256 after; TSan 0/8 hangs, 0 warnings |
| P4 no-op ripple move | `RippleMove` refuses a landing equal to the clip's un-extended position; the controller skips the drop on the clip's own trailing cut | Core test; the fuzz test now fails any edit that leaves every clip where it was but loses a dissolve |
| P5 thumbnail churn | Requested frames snap to a power-of-two grid chosen from the zoom; each snapshot starts a request generation and the worker drops queued frames the view stopped asking for | Engine test: 2 of 120 swept frames decoded; `zoom.py`: no decodes after the zoom ends |
| P6 short ripple drag | In ripple mode, a drop inside the clip's own old span is no move (preview and release agree) | Controller test |
| P7 `EditMarker` merge | Merges only within one gesture id; a merge that ends as a no-op drops its entry (`Command::isNoOp()`) | Core test |
| P8 undo mid-drag | Covered by P1's fix: the drag is cancelled when the undo stack changes | By construction |
| P9 `just asan` red | Timing checks are reported, not checked, under ASan/TSan (`__SANITIZE_ADDRESS__`/`__SANITIZE_THREAD__`) | Code |
| P10 popover warnings | The media browser's menu is unparented when its panel is destroyed (the timeline's are, by `UsTimelineView`'s dispose) | App session: 0 warnings on close |

Found while fixing, by the fuzz test with the P4 invariant and undo/redo checks:

- `MoveClip` had P4's bug too: landing on its own un-extended position only lost the dissolve. Refused now.
- `AddTransition` allowed a dissolve as long as a whole clip, which started both clips on the same frame; a later re-sort could then flip them. Refused now (strictly shorter), and `Model` breaks sort ties by end then id, so a project saved with one still orders stably.

Worth reporting upstream (MLT 7.40, `consumer_sdl2_audio.c`): besides P3, the consumer thread's paused path checks `running` outside `refresh_mutex` and then waits on `refresh_cond` without re-checking it, so a stop that lands between the two is a lost wake-up. Not reproduced here.

Also worth reporting (MLT 7.40, found during doc 19 MT1):

- `mlt_factory.c` assigns every service's `_unique_id` with a plain `++unique_id` on a static int (line 306), so producers created on two threads at once can get the same id. That's a data race, but a benign one for us, since nothing reads `_unique_id`. The thumbnail and waveform workers could already hit it before MT1. TSan doesn't report it under the justfile's `ignore_noninstrumented_modules`.
- `transition_mix.c` embeds its two 192,000-sample × 6-channel float
  buffers in the struct (lines 37–38), a 9.2 MB `calloc()` per mix, one
  per dissolve. Under glibc's dynamic mmap threshold these end up
  heap-allocated and zeroed in full: 5.7 s rebuilds and 36 GB resident at
  1,992 dissolves (doc 19, MT2 piece 0). We pin `M_MMAP_THRESHOLD` in
  `FactoryPolicy`. Upstream could allocate the buffers lazily, or size
  them to the frame's real channels and samples.
- The loader's `dictionary` and `normalizers` and avformat's `avformat_initialised` are initialised lazily with no lock (`producer_loader.c:87`, `:207`; `avformat/factory.c:56`). Parallel probes crashed in `attach_normalizers` about 1 run in 7. We work around it by warming them up in `FactoryPolicy` (`606d171`); upstream could use a once-guard.

## What was run

| Run | Result |
|---|---|
| `meson test` (plain) | 19/19 pass |
| `just asan` (ASan + UBSan) | 18/19. `app-timeline-render` fails its 60 ms budget at 253 ms (P9). No ASan, UBSan or LSan reports from our code |
| `just tsan` | 18/19. `engine-playback-controller` hit the 600 s timeout: the deadlock in P3. No ThreadSanitizer warnings in any suite |
| App, ASan, `gestures.py` + `mods.py` | 26 scripted steps: plain, Ctrl, Shift and Alt drags on clips and edges, rubber band, ripple-mode moves, nudges, Tab walk, markers, zoom, split, ripple delete, 30-step undo/redo, and edits during playback. No ASan or UBSan errors. The only leaks are Mesa's EGL/X11 driver at `gtk_init` (the same 77,303 bytes every run) |
| App, `stale_selection.py` | P1: aborted on `Model::clip: unknown ClipId` |
| App, `render_quit.py` | P2: ASan SEGV, then 3/3 SIGSEGV with the plain build |
| App, `nudge_play.py` (plain) | 242 consumer restarts while playing, at idle and with 16 busy cores; no hang (P3) |
| App, `zoom.py` (plain) | P5 counts |
| `test_playback_controller`, one case | 1/8 hangs under TSan; 0/30 plain at idle; 5/64 plain with 16 run in parallel (P3) |

**How the app sessions ran:**

- Under Xvfb (X11), because AT-SPI's synthetic input doesn't reach GTK on
  Wayland.
- Inside `dbus-run-session`, with scratch XDG directories.
- With `SDL_AUDIODRIVER=dummy`, so nothing played through the owner's
  speakers.

## P1. Removing an asset leaves stale clips in the selection (High, reproduced)

`onRemoveAssetClicked()` and the "Move to Trash" callback run `RemoveAsset`,
which removes every clip that uses the asset. Neither touches the timeline
selection, so the removed clips' ids stay selected.

`onRippleDeleteSelected()` sorts the selection with a comparator that calls
`m_model.clip(id)` (`app_window.cpp:3837`). With two or more ids selected and
one of them stale, that comparator asserts. In a release build it would
dereference `end()` instead.

Reproduced in the app (`stale_selection.py`):

1. Recover the test project and press Ctrl+A.
2. In the media browser, right-click `still.png` and choose Remove from
   Project.
3. Press Shift+Delete. The app aborts:

```text
../src/core/model/model.cpp:105: const ustudio::core::Clip& ustudio::core::Model::clip(ustudio::core::ClipId) const:
Assertion `it != clips.end() && "Model::clip: unknown ClipId"' failed.
```

With a single stale id there is no crash, but every selection edit refuses:

- Delete fails: `RemoveClip` refuses the stale id, so the whole composite
  is refused.
- Nudge fails: `MoveClips` refuses.
- A group drag fails.

Each of these stays refused until the selection is changed.

**Fix:**

- Prune the selection whenever the model changes, from one place.
  Connecting `m_undoStack.changed` (it already drives the title) to
  `m_timelineController.selection().prune(m_model)` covers `RemoveAsset`
  and any future clip-removing command.
- `applyTimelineOutcome()`'s own prune can then go.
- `onRippleDeleteSelected()` should also skip ids the model doesn't have
  before sorting.

## P2. Quitting during a render crashes (High, reproduced)

`onRenderFinished()` runs `renderProject()` on a detached `std::thread`
(`app_window.cpp:1533`). When the window closes:

- `main()` returns.
- `FactoryPolicy`'s destructor calls `Mlt::Factory::close()`, which unloads
  MLT's modules.
- The render thread is still executing inside the avformat consumer at that
  point.

ASan caught a SEGV on a read at an address equal to the program counter:
the thread jumped into unmapped code. The report was cut short because the
process was exiting. With the plain build, a render of the test project
closed 0.3 s after starting crashed 3 of 3 times (exit −11). Each crash left
`export.mp4.part` beside the chosen output, which nothing ever cleans up.

Quitting after the render finished was clean (2/2), so it's the overlap.
The same would happen with SIGTERM or logout during a render, because
`onQuitSignal` goes through the same shutdown.

**Fix:**

- Make the render thread joinable and owned by `AppWindow`.
- Give `renderProject()` a cancel flag that stops its consumer.
- In `onCloseRequest()`, ask "A render is running. Stop it and quit?".
- In `prepareForShutdown()`, cancel and join the render before returning,
  so `Factory::close()` stays last (CLAUDE.md's threading rules).
- Delete the `.part` file when a render is cancelled or fails.

A render queue is M6 territory. The join and the cancel flag are enough
now.

## P3. Consumer stop can deadlock inside MLT (Medium, reproduced in the test)

**Symptom.** `engine-playback-controller` hung in "repeated setTractor calls
on a live, running consumer" in the TSan suite run. It sat for 600 s after
two consumer starts. Backtraces from a hung plain build (with TSan the
stack is the same):

```text
main thread     pthread_join <- sdl2 consumer_stop <- mlt_consumer_stop <- PlaybackController::shutdown <- setTractor
consumer thread pthread_cond_wait <- mlt_consumer_rt_frame <- sdl2 consumer_thread
read-ahead      pthread_cond_wait <- consumer_read_ahead_thread
```

Our main thread is joining the sdl2 consumer thread. That thread waits in
`mlt_consumer_rt_frame` for a frame from the read-ahead thread, and the
read-ahead thread is itself waiting. Nothing wakes them.

**Rates:**

| Build | Conditions | Hangs |
|---|---|---|
| Plain | At idle | 0/30 |
| TSan | Isolated | 1/8 |
| Plain | 16 copies in parallel | 5/64 |

- Every plain hang came on the first stop after `setTractor()` +
  `pause()` + `play()`. That is the stop of a consumer only a few
  milliseconds old.
- Skipping the purge in that first `pause()` (sanitizer report S4's idea)
  reduced the rate to 1/64 but did not remove it. So the purge widens the
  window rather than causing it.

**In the app.** `nudge_play.py` made 242 consumer restarts while playing,
at idle and again with every core busy, and none hung. Each real rebuild
takes about 100 ms, so the consumer is older when it's stopped. The
exposure is low but real: if a stop lands in that window, the UI freezes
for good. Meanwhile the test suite flakes under load.

**Next step.**

- Read MLT 7.40's `mlt_consumer.c` and `consumer_sdl2_audio.c` start/stop
  ordering against these stacks before choosing a fix. Neither was
  available on this machine.
- Candidates to test:
  - stopping the read-ahead before joining the sdl2 thread;
  - not stopping a consumer until it has shown its first frame;
  - for the test only, sleeping one frame between restarts.
- Worth an upstream report with the stacks.
- This belongs with ADR-016's engine-thread work (MT1), where playback
  control moves off the main thread anyway.

## P4. A no-op ripple move deletes dissolves (Medium, reproduced)

`RippleMove::perform()` strips every dissolve on the moved clip before it
knows whether the clip will end up anywhere new. `RippleMove::apply()` only
refuses `m_pos == clip.position`, measured against the extended position.

In ripple mode, a drop inside another clip snaps to that clip's nearer
edge. Dropping clip B anywhere in the first half of the next clip C snaps
to C's start. `RippleMove(B, track, C.position)` then:

1. closes B's gap, which moves C back to B's old start;
2. puts B back in front of C.

The layout is unchanged, but the A→B dissolve is gone, and the result is
an undo step.

`ripple_noop.cpp`, with clips A, B and C butted and a 20-frame A→B dissolve:

```text
ripple B -> C.start: applied; B at 100 C at 200; transitions=0; model unchanged=0
```

This is the same bug audit C3 fixed for `MoveClip`.

**Fix:** refuse in `RippleMove::apply()` when the track is unchanged and the
effective drop point equals the clip's base position. The effective drop
point is `m_pos`, minus the clip's length when it is at or past the old end.
Also add the case to the timeline fuzz test's invariants: an edit that
leaves every clip's base geometry unchanged must not remove a transition.

## P5. Zooming queues thumbnail decodes without bound (Medium, performance, measured)

`drawThumbnails()` requests the frame at each tile's left edge:
`clip.in + k * step / pxPerFrame`. The tile step is fixed in pixels, so
every zoom level asks for a different set of frames. Pinch zoom produces
many levels.

`ThumbnailCache::frameThumbnail()` queues each new key newest-first, and
nothing ever drops a queued job. The worker decodes all of them, including
levels the view has already left.

Measured with `zoom.py` on the test project (four short 1080p clips):

| Phase | Decode jobs | Worker time |
|---|---|---|
| Recovered view, at rest | 6 | 0.46 s |
| 12 zoom-ins, then 12 zoom-outs | 102 | 3.6 s |

Those clips decode in about 35 ms per frame. Long-GOP 4K seeks are
100–300 ms, so the same gesture on real footage keeps a core busy for tens
of seconds. The 800-entry cache churns while it does.

**Fix:**

- Snap requested frames to a zoom-independent grid, for example the
  nearest multiple of a power-of-two frame step chosen from the zoom, so
  nearby zoom levels share keys.
- Give each snapshot a generation number, and have the worker skip queued
  frame jobs from older generations.

## P6. A short ripple drag previews valid, then is refused (Low, reproduced)

**The case.** In ripple mode, drag clip B a little to the right, so the drop
point stays inside B's own old span. The next clip C is butted against B.

**What happens:**

- `TimelineController::motion()` checks the drop against the other clips
  as they are now. Nothing covers it, so the preview draws valid.
- `RippleMove` evaluates it against the closed-up timeline, where C has
  moved into that spot, and refuses: "that's inside another clip".
- `ripple_short.cpp` shows the refusal (`ripple B -> 150 ... REFUSED`).

**Fix:** have the controller apply the same close-up adjustment when it
validates and snaps the drop, so the preview matches the command.

## P7. `EditMarker`: a merged no-op can't redo (Low, latent)

**The bug.** `EditMarker::mergeWith()` absorbs any later `EditMarker` for
the same marker. A drag that returns to where it started merges into a
command whose target equals its origin. Undo then does nothing. On redo,
`apply()` returns false because nothing changes, and `UndoStack::redo()`
asserts (`marker_redo.cpp`).

`mergeWith()` also has no gesture boundary, so two separate edits of one
marker become a single undo step unless a save falls between them.

**Why it's latent.** Nothing in the app issues `EditMarker` yet. It will
bite when marker dragging lands.

**Fix:**

- Merge only within one gesture (an explicit gesture id, or a flag the
  controller clears on release).
- Drop a merged entry that has become a no-op instead of keeping it.

## P8. Undo or redo mid-drag leaves the drag live (Low, by inspection)

`TimelineController::cancel()` is documented as "Escape, or the model
changed under the drag", but only Escape calls it. `onUndo()` and `onRedo()`
clear the selection but leave the drag running.

The drag's origin (`m_originStart`, `m_originLength`) is then stale. A trim
released after Ctrl+Z builds `ResizeClip(clip, clip.in + delta, …,
m_preview.start)` against the new state. That can move the clip back to
where the undone edit had it. Commands still validate, so this can't
corrupt the model, only surprise.

**Fix:** call `m_timelineController.cancel()` when the undo stack changes
while a drag is active.

## P9. `just asan` is always red (Low, tooling)

`test_timeline_render.cpp:115` checks a 10-track zoomed snapshot against a
60 ms budget. It takes 3–6 ms plain, 33 ms under TSan and 254 ms under
ASan. As a result
`just asan` fails on every run, and a real ASan failure there would be easy
to overlook.

**Fix:** skip or scale the timing checks when `__SANITIZE_ADDRESS__` or
`__SANITIZE_THREAD__` is defined.

## P10. Popovers are never unparented (Low)

Three popovers are attached with `gtk_widget_set_parent()` and never
unparented:

- the media-browser context menu (on `m_mediaBrowserPanel`);
- the track context menu (on the timeline);
- the inline name editor (on the timeline).

Every window close logs:

```text
Gtk-WARNING: Finalizing GtkScrolledWindow …, but it still has children left: GtkPopover …
```

It appeared on 7 of 7 sessions that closed the window.

**Fix:** unparent them in the window's destroy path.

## Checked and found sound

- **New core commands.** `ShiftClips`, `MoveClips`, `RippleDelete`,
  `RippleTrim`, `SlipClip` and `CopyClip`:
  - validation happens before mutation;
  - revert order is right;
  - redo reuses the same step objects, so ids are stable.
  - `MoveClips` and `RippleMove` do their dry runs on a model copy.
    `Model`'s copy constructor deliberately doesn't carry `changed`
    subscribers, so a dry run never triggers an engine rebuild.
- **Primitives.** The C1–C4 follow-ups in `primitives.cpp` are correct:
  `RemoveAsset` captures after stripping; `ResizeClip` and `SplitClip`
  strip only the moving edge; revert repoints the split's outgoing
  dissolve. `Model::addTransition` keeps id order, so undo restores the
  vector exactly.
- **Project format 4.**
  - The reader finds the sequence tractor by `ustudio:format_version`, not
    by position, and still reads format 3.
  - Generator assets round-trip through `ustudio:path`.
  - Render playlists reuse `core::planTrackSegments()`, the same planner
    EngineSync uses.
- **Engine.**
  - Stream switches now live on per-variant masters, since MLT ignores
    `video_index`/`audio_index` on a cut.
  - `Tractor::field()` and `track()` wrappers are owned.
  - The black master is created once, with a fixed length, and never
    mutated.
  - Both caches post through `MainThreadDispatcher` with lifetime tokens.
  - `MarkersChanged` skips rebuilds.
- **Signals.** SIGTERM and SIGINT now quit through GApplication: the app
  logged "Received SIGTERM/SIGINT", autosaved and exited 0.
- **Autosave on Discard.** The autosave written after Discard on close is
  deliberate (audit A2's safety net), not a bug.
- **Timeline math.** Viewport and row-layout math: no division by zero,
  since `pxPerFrame` is clamped to 0.005 or more.

## Not done

- **Real media.** No 4K or long-GOP footage in the app sessions; P5's scale
  is extrapolated from per-frame decode costs.
- **Pinch zoom and kinetic scrolling.** No synthetic touchpad gestures on
  Xvfb.
- **Wayland-only paths.** The document-portal path resolution
  (`portal_path.cpp`) was read, not exercised. The Xvfb sessions used GTK's
  in-process file chooser.
- **TSan app session.** TSan ran over the suites only.
