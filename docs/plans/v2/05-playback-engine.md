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

Selection is by trying `Mlt::Consumer(profile, name).is_valid()` in order,
same as kdenlive `:780-796`. The chosen backend name is stored in settings.

## Consumer configuration

| Property | Value | Why |
|----------|-------|-----|
| `real_time` | `1` (drop frames) by default; `-1` to disable dropping (preference) | `>0` keeps real time by dropping; magnitude = decode threads. Start with 1 thread; expose 2–4 as a preference for 4K. |
| `mlt_image_format` | `rgba` | Matches `GDK_MEMORY_R8G8B8A8` with no conversion in our code. yuv420p + shader upload is a later optimisation (doc 13). |
| `channels` | 2 | Stereo monitoring. |
| `frequency` | 48000 | |
| `scale` | `0.5` while playing at 1080p+, `1.0` when paused | Half-res preview during playback halves decode/upload cost; paused frame is full quality. Preference: Auto/Full/Half/Quarter. |
| `volume` | user | |
| `buffer` | 25 (frames) | Default MLT prefetch; lower (12) for snappier reverse/scrub. |
| `terminate_on_pause` | 0 | Consumer stays alive across pause; we pause by speed 0. |
| `audio_device` (sdl) | from preferences if set | |

Width/height are the profile's. Do **not** set consumer width/height to the
widget size; use `scale` instead so the aspect and pixel maths stay in one
place.

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
- `pause()`: `tractor.set_speed(0)`; `consumer.purge()` to flush the
  prefetch buffer so the displayed frame is the one at the playhead, then
  `consumer.set("refresh", 1)` to render exactly one frame. This is the
  kdenlive pattern and is what gives frame-accurate pause.
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
