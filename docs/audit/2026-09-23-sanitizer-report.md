# Sanitizer report, 2026-09-23

First run of AddressSanitizer, UndefinedBehaviorSanitizer and
ThreadSanitizer over the project, at `bb0026f` (v0.14.7). Written for the
owner and the code team. Runtimes: `libasan`, `libubsan`, `libtsan`
16.2.1 with GCC 16.1.1 (the minor-version mismatch caused no problems).
Nothing in `src/` was changed; the scripts used are in
[`2026-09-23-sanitizer-run/`](2026-09-23-sanitizer-run/).

## Bottom line

- **No memory-safety errors and no undefined behaviour anywhere**: not in
  the test suite, not in 120 repeated runs of the threaded tests, and not in
  a scripted 90-second app session that recovered a project, played it,
  looped, shuttled, and edited during playback.
- **One real, growing memory leak (S1)**: every edit leaks the previous
  tractor's transition graph, about 100–330 KB per edit.
- **The app ignores SIGTERM (S2)**, because SDL installs its own handler.
  Logout, shutdown or `kill` cannot stop it cleanly.
- Two smaller MLT-side issues we trigger (S3, S4) with cheap workarounds,
  and a set of ThreadSanitizer false positives worth silencing so future
  runs are useful (S5).

## What was run

| Run | Build | Result |
|---|---|---|
| Full test suite (10 targets) | ASan + UBSan | 0 memory errors, 0 UB. 8 engine targets exit non-zero **only** for leak reports at exit |
| 6 threaded engine tests × 20 runs, leak check off | ASan + UBSan | 120/120 pass, 0 reports |
| Full test suite | TSan | 5 targets report; all classified below, none real in our code |
| Scripted app session (driver below) | ASan + UBSan, leak check on | 0 errors, 0 UB; 18.6 MB leaked at exit, traced to S1 |
| Same session | TSan, `ignore_noninstrumented_modules=1` | 0 reports |
| SIGTERM timing | ASan build | Ignored for 15 s then killed; exits in 0.1 s with SDL's handlers off |

The app session ran on a private D-Bus session bus with scratch XDG
directories, so it could not reach a running editor or touch real
autosaves, logs or recent files. It recovered a generated project (two clips
with a dissolve, a PNG still on V2, an audio clip on A1; test media rendered
by MLT, no real footage), then drove the window's exported actions:
play and pause, J/K/L shuttle both ways, frame steps, previous/next cut,
10-frame and one-minute jumps, loop in/out with looped playback, track
switching, split, add track, 16 undo/redo steps while playing, delete, the
media browser with thumbnails, New Project (the `EngineSync::reset()` path),
playback on the empty project, and closing the window. The app log confirms
each step took effect (28 plays, 28 consumer starts, splits and undos
reported in the status line).

## Findings

### S1. Every edit leaks the old transition graph (High, verified)

`Mlt::Tractor::field()` returns a **new** `Mlt::Field` wrapper on every
call, and that wrapper holds a reference on MLT's field. mlt++'s header
says "Caller does not own the result"; that comment is wrong. EngineSync
calls it inline and never deletes it:

- [engine_sync.cpp:458](../../src/engine/engine_sync.cpp#L458) and
  [:464](../../src/engine/engine_sync.cpp#L464), twice per adjacent track
  pair in `rebuildAll()`;
- [engine_sync.cpp:343](../../src/engine/engine_sync.cpp#L343) and
  [:359](../../src/engine/engine_sync.cpp#L359), twice per dissolve in
  `buildTransitionSubTractor()`.

Because the wrapper keeps the field alive, every `composite` and `mix`
planted in every rebuilt tractor is never freed, including the `mix`
transitions' audio buffers. In the app session, 9.2 MB of the 18.6 MB
leaked sat under the `mix` transitions created at line 460 (43 of them in
about 28 rebuilds). A further 8.4 MB single `mlt_pool_alloc` block has no
frame of ours and is probably a frame buffer the leaked transitions retain;
recheck it after the fix.

Standalone repro (`rebuildrepro.cpp`: black track plus three playlists,
`composite` + `mix` per pair, three frames pulled per rebuild):

| Rebuilds | RSS, wrapper leaked | RSS, wrapper deleted |
|---|---|---|
| 100 | 80 MB | 95 MB |
| 200 | 90 MB | 95 MB |
| 300 | 101 MB | 95 MB |

Rebuilds happen on every edit, so a long editing session grows by roughly
100–330 KB per edit (the app's figure is higher because real audio flows
through the mixes): on the order of 100–300 MB over a thousand edits.
Deleting the wrapper was checked with ASan to be safe (no double free, the
tractor stays valid).

Fix: take the field once per tractor and own it.

```cpp
std::unique_ptr<Mlt::Field> field(newTractor->field());
field->plant_transition(composite, index - 1, index);
field->plant_transition(mix, index - 1, index);
```

The same wrong header comment is on `Tractor::track(int)`: 100 calls leak
about 10.7 KB, and deleting the result is safe (ASan repro). It is only
used in `EngineSync::verify()`
([engine_sync.cpp:491](../../src/engine/engine_sync.cpp#L491)), a test
path, so it is low impact; the 2026-09-20 audit's E7 flagged it and the
fix commit set it aside because of that comment. Worth a line in the
README's MLT notes: *mlt++ accessors that return a pointer allocate a new
wrapper; delete it*, whatever the header says.

### S2. The app ignores SIGTERM (Medium, verified)

When MLT's `sdl2_audio` consumer initialises SDL, SDL installs handlers
for SIGINT and SIGTERM that turn them into an `SDL_QUIT` event nobody
reads. Measured with the app idle:

| Environment | Response to SIGTERM |
|---|---|
| default | still running 15 s later; needed SIGKILL |
| `SDL_NO_SIGNAL_HANDLERS=1` | exited in 0.1 s |

Consequences: logging out or shutting down waits for the session manager's
timeout and then kills the app, with no final autosave; `kill <pid>` and
Ctrl+C in a terminal do nothing. The same thing orphaned a test instance
during this run.

Fix: in `main()`, before `FactoryPolicy`, `g_setenv("SDL_NO_SIGNAL_HANDLERS",
"1", FALSE)`; then handle SIGTERM and SIGINT with `g_unix_signal_add()` so
they autosave when dirty and call `g_application_quit()`, which runs the
existing shutdown path.

### S3. MLT's loader leaks about 4.6 KB per producer (Low, verified, upstream)

Creating a producer through MLT's loader (`"colour:red"`, or any file path)
and destroying it leaks the normaliser filters the loader attaches: 100
create-and-destroy cycles leaked 458 KB above the fixed baseline, while
naming the service directly (`Mlt::Producer(profile, "colour", "red")`)
leaked nothing. We can't avoid the loader for media files, but
`rebuildAll()` creates a new black backing producer through it on every
edit ([engine_sync.cpp:416](../../src/engine/engine_sync.cpp#L416)).
Create that one with the explicit service, or cache it like the master
producers. Probe, waveform and thumbnail jobs leak once per job, which is
acceptable; worth an upstream MLT report with the repro.

### S4. A data race inside `mlt_consumer_purge` that we trigger on every edit (Low, verified in MLT source)

ThreadSanitizer caught our main thread locking MLT's consumer queue mutex
while the consumer thread was still initialising it. In MLT 7.40's
`mlt_consumer.c`, `consumer_read_ahead_start()` (consumer thread) inits
`queue_mutex` and then sets a plain `int started = 1`;
`mlt_consumer_purge()` (our thread) reads `started` without
synchronisation and then locks the mutex. On x86 the store order makes
this harmless in practice; on ARM it could lock a half-initialised mutex.
We hit the window because `setTractor()` calls `pause()`, which purges,
immediately after starting a fresh consumer, on every edit. The queue is
empty at that point, so skipping the purge after a fresh start (seek and
refresh only) removes the race for free. Worth an upstream report.

### S5. ThreadSanitizer false positives to silence (Info)

All remaining TSan reports that touch our code are one pattern: a worker
thread allocates a closure and hands it to the main thread through
`g_idle_add`. GLib's main-context lock is futex-based and invisible to
TSan, and the tests' main thread polls with short sleeps, which TSan labels
"as if synchronized via sleep". The real app, which blocks in GLib's main
loop and is woken through a file descriptor, produced none of these.

| Reported at | Verdict |
|---|---|
| `dispatcher.cpp:37`, `:38`; `playback_controller.cpp:386` | false positive (hand-off through `g_idle_add_full`) |
| `waveform_cache.cpp:148`; `thumbnail_cache.cpp:165` | false positive (hand-off through `g_idle_add`) |
| `tests/engine/test_waveform_cache.cpp`, `test_thumbnail_cache.cpp` callback lines | false positive (same hand-off) |
| GLib `g_free`, glycin (image loading), x264, MLT `pool_close` / `mlt_property_get_data`, PipeWire `libspa-support` lock-order inversions | third-party, not actionable here |
| Thread leaks in `test_probe_media`, `test_render`, `test_thumbnail_cache` | threads created inside MLT modules (unloaded before the report), third-party |

To make future TSan runs signal-only: mark the hand-off in
`MainThreadDispatcher::post` with `__tsan_release(ctx)` before
`g_idle_add_full` and `__tsan_acquire(ctx)` in the callback (from
`<sanitizer/tsan_interface.h>`, compiled only under
`__SANITIZE_THREAD__`), route the two caches through
`MainThreadDispatcher` instead of raw `g_idle_add`, and keep a small
`tsan.supp` for the third-party rows above.

### Leaks at exit that are not ours (Info)

| Source | Size | Note |
|---|---|---|
| `Mlt::Factory::init` | 14,375 B, fixed | MLT's repository; identical in every process |
| fontconfig / Pango (`FcPattern*`, `pango_cairo_show_layout`) | ~0.7 MB | font caches |
| SDL (`SDL_CreateMutex`, `SDL_malloc`) under `consumer->start()` | < 10 KB | audio subsystem never shut down |
| `AppWindow` and what it owns | ~92 KB | intentionally leaked on quit (main.cpp) |
| per-producer loader filters | see S3 | |

## Not covered

- Timeline mouse gestures (drag, trim, right-click menus, transition
  resize): the timeline is one custom drawing area, which AT-SPI can't
  target by element. The same commands are covered by the core and engine
  tests.
- File dialogs (import, save, render from the UI): they go through the
  desktop portal, which the private session bus doesn't provide. Save and
  render are covered at engine and core level.
- The two-minute autosave heartbeat (the session was 90 seconds).
- TSan on the full app without the uninstrumented-module filter: GTK and
  GLib produce hundreds of false positives there.

## Recommendations

1. Fix S1 now; it is a few lines and the largest effect.
2. Fix S2 in `main.cpp` (environment variable plus signal handlers).
3. Take the S3 and S4 workarounds when next in `EngineSync` and
   `PlaybackController`; file both upstream with the repros.
4. Make sanitizer runs routine: add `just asan` and `just tsan` recipes that
   configure `builddir-asan` / `builddir-tsan`, with an `lsan.supp`
   covering the "not ours" rows and the S5 changes, so the suites pass
   clean under sanitizers and any new report is a real one. Rerun the app
   session with the scripts in `2026-09-23-sanitizer-run/` after S1 to
   confirm the exit leak drops from 18.6 MB to roughly the fixed baseline.

## How to rerun

```sh
meson setup builddir-asan -Db_sanitize=address,undefined -Db_lundef=false
meson setup builddir-tsan -Db_sanitize=thread -Db_lundef=false
meson compile -C builddir-asan && meson compile -C builddir-tsan

ASAN_OPTIONS="log_path=/tmp/asan/asan:detect_leaks=1:detect_stack_use_after_return=1" \
UBSAN_OPTIONS="log_path=/tmp/asan/ubsan:print_stacktrace=1" \
  meson test -C builddir-asan -t 6

TSAN_OPTIONS="log_path=/tmp/tsan/tsan:second_deadlock_stack=1" \
  meson test -C builddir-tsan -t 10
```

App session (from the artifacts folder, with scratch XDG directories and
the generated project copied into `$XDG_STATE_HOME/ustudio/autosave/`):

```sh
XDG_STATE_HOME=… XDG_DATA_HOME=… XDG_CACHE_HOME=… XDG_CONFIG_HOME=… \
ASAN_OPTIONS="detect_leaks=1:log_path=/tmp/app/asan" \
  dbus-run-session -- ./session.sh ./drive.py \
  builddir-asan/src/app/u-studio-video-editor /tmp/app full
```
