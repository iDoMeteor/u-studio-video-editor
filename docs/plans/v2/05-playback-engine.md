# 05 — Playback engine

Replaces the hand-rolled pull loop and PulseAudio simple API with an MLT
consumer that owns the clock (ADR-002). Reference implementation of the
pattern: `~/Repos/kdenlive/src/monitor/videowidget.cpp:765-880`
(`reconfigure`) and `:1080` (`on_frame_show`). We reuse the *approach*, not
the code.

## Components

```
Model ──(events)──▶ EngineSync ──owns──▶ Mlt::Tractor (+ black track, transitions, filters)
                                              ▲ connect()
PlaybackController ──owns──▶ Mlt::Consumer("sdl2_audio")
        │                          │ consumer-frame-show (MLT thread)
        │                          ▼
        │                    LatestFrameSlot ── main loop ──▶ PreviewPane (GdkMemoryTexture)
        ▼
  positionChanged / stateChanged signals (main thread)
```

## Consumer selection

At startup (`FactoryPolicy`), and again if the user changes the audio backend
in preferences:

1. `sdl2_audio` (preferred; audio-only variant of the SDL consumer; drives
   PipeWire/Pulse via SDL; present here via `sdl2-compat`).
2. `rtaudio` (fallback; also present).
3. `null` with `real_time=1` (last resort: no sound, but the consumer still
   paces to the profile fps so playback remains usable; a persistent banner
   says "Audio output unavailable").

> REVIEW: Claude (2026-09-24): measured, `null` does **not** pace (9,000+ frames a second in
> a device-less container), and with no audio device `rtaudio`'s `start()`
> returns 0 and then never shows a frame, so it gets picked and playback
> stalls (`playback_controller.cpp`). SDL's `dummy` driver
> (`SDL_AUDIODRIVER=dummy`) is silent and paced; the tests use it. The banner
> and storing the backend in settings are not implemented.

Selection is by trying `Mlt::Consumer(profile, name).is_valid()` in order,
same as kdenlive `:780-796`. The chosen backend name is stored in settings.

## Consumer configuration

| Property | Value | Why |
|----------|-------|-----|
| `real_time` | `1` (drop frames) by default; `-1` to disable dropping (preference) | `>0` keeps real time by dropping; magnitude = render threads. **Values above 1 measured broken with `sdl2_audio`** (2026-09-24: 5–6 frames/s shown at 4K60 instead of 20+), so don't expose them until that is understood; see the multi-threading plan. |
| `mlt_image_format` | `rgba` | Matches `GDK_MEMORY_R8G8B8A8` with no conversion in our code. yuv420p + shader upload is a later optimisation (doc 13). |
| `channels` | 2 | Stereo monitoring. |
| `frequency` | 48000 | |
| `volume` | user | |
| `buffer` | 25 (frames) | Default MLT prefetch; lower (12) for snappier reverse/scrub. |
| `terminate_on_pause` | 0 | Consumer stays alive across pause; we pause by speed 0. |
| `audio_device` (sdl) | from preferences if set | |

Width/height are the profile's. Neither the consumer's `scale` nor its
width/height is used for preview scaling; see below.

## Preview scale

Preview scale (Auto/Full/Half/Quarter) is applied by building the
**playback tractor on a scaled profile**: `EngineSync` takes the sequence
profile, multiplies width and height by the factor (rounded to even
numbers) and keeps fps, SAR and DAR, so positions and aspect are
unchanged. Export (`renderProject`) and media probing always use the full
profile.

Measured on 2026-09-24 (4K60 H.264 source, `tests/engine/playback_soak`),
because the first design did not work:

| Approach | Frames shown (of 60/s) | Frame the UI received |
|---|---|---|
| Consumer `scale = 0.5` (the original plan) | 20–23 | 3840×2160: no effect |
| Consumer width/height set to half | ~16 | 1920×1080, but slower: rendering stays full size and one more downscale is added |
| Half-size playback profile (chosen; kdenlive resizes its monitor profile too) | 32–43 | 1920×1080 |

- **Auto** is Half for sequences taller than 1080 lines, Full otherwise.
  It no longer changes between playing and paused: a factor change
  rebuilds the tractor and restarts the consumer, which is too costly for
  every play/pause. So a paused 4K frame also shows at half size. A
  full-quality still for the paused frame (rendered off-thread, like
  thumbnails) is a follow-up.
- Changing the preference rebuilds only when the resolved factor changes.
- Anything with **pixel-valued parameters** (effects in doc 15, titles in
  doc 16) must scale them with the playback profile or use relative units.
- **Decoder threads**: the `avformat` producer's `threads` property is
  documented as default 1, but unset it lets FFmpeg pick (about one thread
  per CPU here). Explicit values (1, 2, 4, 8) made no measurable
  difference in repeated runs, so the engine leaves it unset.

## State machine

```
Stopped ──start()──▶ Paused ◀──────────┐
                        │  play(speed)  │ pause()
                        ▼               │
                     Playing(speed) ────┘
   any ──seek(f)──▶ (same state; if Paused → consumer "refresh"=1 to show one frame)
   Playing ──end reached──▶ Paused at last frame (or loops if loop range set)
```

Implementation notes (all main thread):

- `play(speed)`: `tractor.set_speed(speed)`; if consumer not started, `start()`.
  Speed ∈ {±0.25, ±0.5, ±1, ±2, ±4, ±8} for J/K/L shuttle; `sdl2_audio`
  handles reverse and pitch-free speed change itself (audio is muted by MLT
  above |2| unless `pitch` handling is enabled; leave MLT defaults).
- `pause()`: `tractor.set_speed(0)`; `tractor.seek(displayed)` back to the
  frame last shown by `consumer-frame-show`; `consumer.purge()` to flush
  the prefetch buffer; then `consumer.set("refresh", 1)` to render exactly
  one frame. This is the kdenlive pattern (`VideoWidget::pause()` seeks the
  producer to the consumer's position before purging) and is what gives
  frame-accurate pause. The seek is not optional: while playing, the
  read-ahead thread has pulled the producer up to `buffer` frames past the
  screen, and `purge()` does not move it back. Without the seek, pause
  landed ~30 frames late (measured 2026-09-20; test added 2026-09-23).
- `seek(f)`: `tractor.seek(f)`; if paused, `purge()` + `refresh=1`. While
  playing, just seek; the consumer catches up.
- Scrubbing (drag on ruler): `seek()` per motion event, rate-limited to one
  outstanding refresh; if a new seek arrives before the previous frame shows,
  the intermediate is skipped (the `LatestFrameSlot` already does this for
  display; on the input side, `PlaybackController` coalesces to the newest
  target).
- `stepFrame(±1)`: pause, then seek(current ± 1).
- Loop: `setLoopRange(in, out)`; on `frame-show` with position ≥ out, seek(in).
  Done on the main thread from the marshalled position event; a one-frame
  overshoot is acceptable for preview.
- **Position source of truth while playing** is the position carried by each
  `frame-show` event; while paused it is what we last sought to. Never read
  `tractor.position()` for display; it runs ahead of what is on screen by the
  prefetch buffer.

## `consumer-frame-show` handler (MLT thread)

```cpp
static void onFrameShow(mlt_consumer, PlaybackController* self, mlt_event_data data) {
    Mlt::Frame frame(Mlt::EventData(data).to_frame());
    if (!frame.is_valid()) return;
    mlt_image_format fmt = mlt_image_rgba; int w = 0, h = 0;
    const uint8_t* img = frame.get_image(fmt, w, h);       // already rgba: no conversion
    if (!img) return;
    auto buf = std::make_unique<uint8_t[]>(size_t(w) * h * 4);   // ONE copy, out of MLT's buffer
    std::memcpy(buf.get(), img, size_t(w) * h * 4);
    self->m_slot.store(FrameData{std::move(buf), w, h, frame.get_position()});
    self->m_dispatcher.postOnce(self->m_token, [self] { self->drainSlot(); });   // at most one queued
}
```

`drainSlot()` on the main thread takes the frame, wraps the buffer with
`g_bytes_new_with_free_func`, builds a `GdkMemoryTexture`, hands it to the
preview, and emits `positionChanged(pos)`. Cost per 1080p frame: one 8 MB
memcpy plus GTK's upload. GTK's own upload can be avoided later with
`GdkGLTextureBuilder`; not in v2.0.

Why copy at all: the `Mlt::Frame` is only valid during the callback, and we
must not block the consumer thread on the main loop.

## Engine graph built by EngineSync

```
Mlt::Tractor
  track 0: color:black  (length = sequence length, updated on change)
  track 1..A: audio tracks  (Mlt::Playlist each; kind=Audio; clips with video disabled: producer.set("video_index", -1))
  track A+1..N: video tracks bottom→top
  transitions (planted per adjacent video track pair): "composite" a_track=lower b_track=upper, always_active=1  (ADR-006)
      – with "affine" used *as a filter on the clip* for transform; composite for blending only
  transitions (audio): "mix" between each audio-bearing track and the one below, always_active=1, sum=1
  clip filters: attached to the clip's cut producer (Effect → Mlt::Filter)
  track filters: attached to the playlist
  clip transitions (dissolves): "luma" planted with in/out over the overlap region on a per-track "mix" sub-tractor
      – v2.0 simplification: dissolves are implemented as a `luma` transition between the two clips'
        producers on a 2-track Tractor that replaces the overlap region on the playlist (kdenlive's
        "mix" model). Full design in doc 08.
```

> REVIEW: Claude (2026-09-24): as built, `composite` and `mix` are planted between every
> adjacent track pair (audio included), and every change rebuilds the whole
> tractor without a lock (`engine_sync.cpp`, `rebuildAll()`).

Rebuild policy (ADR-005): on any model event touching a track, `EngineSync`
rebuilds that **whole track playlist** under the tractor lock: clear it,
append blanks and cuts in order. Cuts come from a per-asset master producer
(`AssetId → Mlt::Producer`), so no file is reopened. Filters are re-attached
from the model. This is O(clips on track) and always consistent. Incremental
updates are an optimisation for later, behind the same `EngineSync`
interface, with the verifier as the safety net.

Verifier (`EngineSync::verify()`, debug builds and tests): for each track,
walk the playlist and compare each non-blank entry's `clip_start`,
`clip_length`, resource, and in/out to the model; compare tractor length to
`sequence.length()`. Any mismatch is a failed assertion with a diff.

## Profile and fps

- `Mlt::Profile` is constructed from `Sequence::profile` (explicit
  width/height/fps/sar/progressive), not from a stock name, so custom
  profiles work. `Profile::mltName` is only a hint for the XML.
- Changing the sequence profile rebuilds everything: stop consumer, destroy
  tractor, rebuild, reconnect. Rare; done via a single `SequenceProfileChanged`
  event.

## Shutdown order

`PlaybackController::~` → `consumer.stop()` (joins MLT threads) → drop the
frame-show listener → `EngineSync::~` → tractor/playlists/producers →
`FactoryPolicy::close()` last, once per process. Never destroy a producer that
a running consumer can still reach.

## What this removes from v1

- `libpulse-simple` dependency and all `pa_*` code.
- The worker thread, `m_seekRequest`, `m_playing`, the sleep-based pacing.
- The A/V offset and the silent-clip drift described in
  [00-v1-review.md § P2](00-v1-review.md#p2-playback-is-a-hand-rolled-clock).
