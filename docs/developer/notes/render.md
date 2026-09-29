# Render notes

[Developer docs](../README.md) › [Implementation notes](README.md)

**A render can be cancelled.** `renderProject()` starts the avformat
consumer and waits on it itself (`Consumer::run()` is just `start()` plus a
wait for "consumer-stopped"), so a cancel flag can stop it from the render
thread; the `.part` is removed. The app owns the render thread, asks before
quitting mid-render, and cancels and joins it before MLT is closed (post-M3
audit P2: quitting mid-render used to crash). A finished render reads
stopped only after avformat has written the trailer and closed the file.
Call `stop()` after that wait even so: with `real_time` -1 the consumer's
render-ahead thread can still be pulling a frame when `run()` returns, and
`stop()` is what joins it. `u-studio-render --frames --ffv1` first skipped
it and crashed every time at exit (SIGSEGV in a filter as
`Factory::close()` unloaded the modules under that thread) or, in a test
that went on using MLT, with a SIGFPE in a filter reading freed memory
(2026-09-29).

**`u-studio-render --frames` (M6 groundwork).** `--frames <project>
[--range IN:OUT]` builds the preview's own graph (Full, frames read at the
profile's size, CPU) and prints a 64-bit FNV-1a hash of each frame's RGBA as
JSON; `engine-render-frames` checks them against the live `Engine`'s frames.
`--ffv1 <output>` renders the range losslessly instead (Matroska, FFV1,
`yuv422p`, PCM): `yuv422p` is the graph's own 4:2:2, so the file decodes to
exactly the YUV an export's consumer gets, which the same test checks byte
for byte.

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
