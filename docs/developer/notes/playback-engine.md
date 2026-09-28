# Playback engine notes

[Developer docs](../README.md) › [Implementation notes](README.md)

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

**…but `play()` pre-rolls a quarter second, and `pause()` puts it back to
1 (0.50.0-beta.3).** With prefill 1, playback after a paused seek played one
frame's audio and then starved while every master decoded from its new spot:
the crackle at the start of playback. Measured through SDL's disk driver
(`SDL_AUDIODRIVER=disk`, a 440 Hz tone, `playback_soak --start 15000
--tracks N`): one track had a 9 ms gap 30 ms in; seven tracks had 9–52 ms
gaps through the first half second. Starting from frame 0 on fresh decoders
was clean. With a pre-roll of 8 frames the gaps were gone, at 80 ms more
start latency on one track and about 340 ms on seven. MLT waits for the
pre-roll only while the speed is non-zero (`mlt_consumer_rt_frame()` uses
size 1 at speed 0), so P3's paused hang stays out of reach.

**Preview scale works by shrinking the playback profile.** Setting the
consumer's `scale` property had no effect on what MLT renders (4K60 at
"Half" still delivered 3840×2160 frames and showed only 20–23 of 60 frames
a second), and overriding the consumer's width/height only added a final
downscale after full-size rendering (slower). `EngineSync` now builds the
playback tractor on the sequence profile scaled by the preview factor
(even dimensions, same fps and aspect); export keeps the full profile.
Auto means Half for sequences taller than 1080 lines, and for any sequence
with a clip whose transform isn't the default (0.47.2; each transformed
1080p track costs 16–22 ms a frame at Full). The dropdown shows what Auto
resolved to, "Auto (Half)". Details and
measurements: doc 05, "Preview scale". Leave the `avformat` producer's
`threads` unset: unset already decodes with about one thread per CPU, and
explicit values measured no better.

**Memory on the CPU path settles; it doesn't leak (2026-09-27).** Three
transformed 1080p tracks at Half: RSS climbs about 35 MB in the first
minute (decoders, frame queues, malloc's per-thread arenas), then stays
within 370–380 MB for the next four minutes. The first consumer restart
(an edit while playing) adds about 60 MB once. After that, 200 edits and
135 restarts left it flat at 420 MB. A one-minute soak under ASan/LSan
(the `just asan` suppressions) reports no leak. The "20 MB/min" seen
earlier compared the first report with RSS read after `shutdown()`, which
stopping the consumer raises by about 50 MB. `playback_soak` now reports
growth after warm-up and the post-shutdown figure separately.
`MALLOC_ARENA_MAX=2` makes it worse (a slow creep), so don't set it.

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
  [`docs/plans/v2/00-v1-review.md`](../../plans/v2/00-v1-review.md) for what
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
  edit (see [Engine sync notes](engine-sync.md)), and an earlier version of `PlaybackController` tried
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
  the exact scenario. Even the stop/restart path has a residual race:
  once in a full ASan suite run (2026-09-27; 0 of 10 isolated repeats)
  that test hit a SEGV in libmlt's `on_consumer_frame_show` →
  `mlt_frame_get_position` on the `sdl2_audio` consumer thread right
  after a restart. The frame the event hands over is already freed.
  That is a candidate for the MLT upstream list, not something our code
  can fix.
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
  Video needed no such tuning: `composite` (with `fill=1`, see "Tracks
  composite with `composite`" in [Engine sync notes](engine-sync.md)) draws the top track over the lower
  ones.
