# Media cache and probing notes

[Developer docs](../README.md) › [Implementation notes](README.md)

## Waveform cache

`WaveformCache` opens its own throwaway `Mlt::Profile`/`Producer` per clip
(never the live tractor's), built with the **sequence's own frame rate**,
not a hardcoded stock profile. Confirmed empirically that this matters:
`Mlt::Producer::get_length()`/`seek()` are normalized to whichever
`Mlt::Profile` the producer was constructed against, not the source file's
native rate — the same file opened at 25fps vs. 50fps reported
`get_length()` of 928 vs. 1857 (not the same number, roughly double,
matching the fps ratio). A clip's `in`/`out` are stored in the sequence's
own fps, so extracting peaks with a mismatched profile would silently seek
every job to the wrong wall-clock position for any project that isn't
exactly that rate. The peak cache is keyed on `(resource, in, out, fps)`
for the same reason — a later project at a different rate must never reuse
another rate's peaks for what would otherwise look like the same clip.

Peak count is capped at 2000 per clip regardless of its length, decoding a
stride of frames instead of every single one above that — `drawWaveform()`
(`app_window.cpp`) already re-buckets whatever's in the peaks array down to
the clip's actual on-screen pixel width, so one peak per video frame on a
long clip was always more resolution than anything ever displayed. Measured
on a real ~62-minute (110,854-frame) recording: 18.2 seconds decoding every
frame before this fix, 2.5 seconds after (stride ≈ 55, ~1980 peaks) — and
that 18 seconds of one CPU core solidly decoding the same file the live
playback consumer was also trying to read from is the leading explanation
for "playback doesn't work" reports that turned out to be "playback is
starved for the fifteen-ish seconds after importing or editing a long
clip," not a hard failure.

## Thumbnail cache

`ThumbnailCache` (`src/engine/thumbnail_cache.{h,cpp}`) is the same
jobs-on-the-pool-plus-cache architecture as `WaveformCache` above (doc 19
MT3: at most a capped number of jobs at once, so imports and probes keep
threads). Pending frames are batched per file: one job opens the producer
once and walks that file's frames, newest first, skipping any the view
stopped asking for. A representative thumbnail is keyed on the resource
path alone (it isn't tied to any sequence's fps the way waveform peaks
are).

**MLT caps live avformat decoders process-wide; raise the cap.** MLT keeps
at most 4 avformat producers' decoder state (`mlt_cache`,
"producer_avformat") and evicts the least recently used when another one
decodes. With cache jobs decoding on pool threads while playback decodes
its own masters, producers evicted each other mid-decode across threads.
Playback crashed in `producer_get_audio → init_cache` (reproduced
2026-09-24; 2 of 2 runs with a 1080p timeline playing while the caches
filled, 0 of 3 after). `FactoryPolicy::raiseAvformatDecoderLimit()` sizes
it as kdenlive does, threads + 2 per track. It is set once before any
other thread exists, and raised from `EngineSync::rebuildAll()`.
Each job opens its own throwaway `Mlt::Profile`/`Producer`, seeks to 10%
into the clip (a plain frame 0 often lands on a fade-in or black open),
decodes one frame, and box-downsamples it in software to a fixed
120px-wide RGBA thumbnail, converted to a `GdkTexture` the same way
`PlaybackController`'s own live-frame callback already does
(`gdk_memory_texture_new(..., GDK_MEMORY_R8G8B8A8, ...)`).

A throwaway `Mlt::Profile` defaults to MLT's own `dv_pal` (720x576,
16:15 sample aspect, 4:3 display) — an audit (2026-09-22) found that the
loader's normalising filters scale and pad every decoded frame to fit
that profile, so `get_image()` returned 720x576 with black letterbox
bars and squashed pixels for any real 16:9 source, regardless of its
actual shape (verified with a standalone repro: a rendered 1920x1080 red
clip decoded to 720x576, with the top/bottom couple of rows reading
black instead of red). Fixed by priming with one throwaway `get_frame()`
first (populating `meta.media.width`/`height`, per the lazy-population
finding under "Media probing" below) and reconfiguring the *same* `Mlt::Profile` object's
width, height, and sample aspect (1:1) before decoding the real
thumbnail frame — confirmed empirically that the producer does not need
to be reopened for this to take effect (`tests/engine/
test_thumbnail_cache.cpp`'s own E2 test renders a real 1920x1080 clip
and checks the thumbnail comes out 120x67, matching the source's real
16:9 shape, not 120x96, dv_pal's).

`AppWindow::onThumbnailReady()` skips its `refreshMediaBrowser()` call
(destroys and recreates every row) when the panel is hidden (audit A4)
— importing N assets at once used to trigger N full rebuilds regardless
of whether the panel was even visible, and it starts collapsed by
default. `onToggleMediaBrowserClicked()` already runs its own
`refreshMediaBrowser()` when the panel goes from hidden to visible, so
opening it afterward still shows everything that finished in the
meantime, just in one rebuild instead of N.

## Media probing

Import also reads an asset's fps and pixel dimensions off the producer's
own `meta.media.frame_rate_num`/`_den`/`width`/`height` properties
(`EngineSync::probeMedia()`) — confirmed empirically (a standalone repro
against a rendered test file, and `tests/engine/test_probe_media.cpp`)
that these are populated **lazily**, only after the producer has actually
decoded at least one frame, not at open time; a still image never sets
them at all (`meta.media.*` is avformat-specific), so they're left at
their zero default there. The asset's recorded "format" (its container)
is read from the filename extension, not any MLT property — `meta.media.*`
has no reliable container/format string to read.

`probeMedia()` also reads whether the asset actually has audio, rather
than guessing "true whenever it isn't a still image" as it used to
(audit E3, 2026-09-22 — the old guess meant a video-only file got
waveform-decode jobs for a silent track and a "Split Audio" menu item
that produced an empty clip). Verified against the avformat producer's
own YAML metadata (`producer_avformat.yml`'s `audio_index`: "Choose the
absolute stream index of audio stream to use (-1 is off)") and a
standalone repro rendering two real MP4s, one muxed with an AAC track
and one without: `audio_index` is auto-detected and set at *open* time
(no frame decode needed first, unlike `meta.media.*` above), -1 exactly
when the container has no audio stream. A `property_exists()` guard
matters too — a non-avformat producer (a generator like `color:`) has
no `audio_index` property at all, and `get_int()` on a missing property
returns `0`, indistinguishable from "stream 0" if read unguarded
(confirmed with the same repro); `tests/engine/test_probe_media.cpp`
covers all three cases (real video with audio, real video without,
generator).
