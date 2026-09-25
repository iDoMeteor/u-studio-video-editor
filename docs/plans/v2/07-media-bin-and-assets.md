# 07 — Media bin and assets

## Flow

```
user picks files (GtkFileDialog, multiple) or drops onto the bin or timeline
  → AddAsset commands (status Pending) → bin shows placeholder cards immediately
  → engine::MediaProbe (worker) opens Mlt::Producer(profile, path), inspects
  → on main thread: Model::updateAssetInfo (NOT a command; probing results are not undoable)
  → EngineSync creates the master producer for the asset
  → thumbnails/waveforms requested lazily by views
```

## Probing (`engine/media_probe.*`)

Worker thread, own `Mlt::Profile` copy (profiles are value-like; construct a
new one from the sequence `Profile` struct). For each path:

1. `Mlt::Producer p(profile, path)`; reject if `!is_valid()`.
2. Reject if `p.get("mlt_service")` is empty or the resource resolves to a
   directory; reject if `get_length() <= 0` unless `mlt_service` is one of
   the still-image producers (`pixbuf`, `qimage` is Qt and excluded, so
   `pixbuf` only) or a generator.
3. Read: `meta.media.width/height`, `meta.media.frame_rate_num/den`,
   `meta.media.sample_aspect_num/den`, `meta.media.nb_streams` and the
   `meta.media.N.stream.type` / `codec.name` entries, `length`, `audio_index`,
   `video_index`, `seekable`. Unseekable sources (some MKV/streams) are
   accepted but flagged; timeline seeks on them are slow.
4. Compute `lengthInSequenceFrames = get_length()` (MLT already expressed it
   in profile frames since the producer was built with the sequence profile).
5. Fingerprint: `size:mtime_ns` from `stat`. Used for cache keys and relink.

Timeouts: a probe taking > 20 s is abandoned (thread-local flag checked by
MLT's own reading via `mlt_events`? No: avformat probing isn't cancellable.
The worker simply moves on and the asset is marked Failed with "timed out";
the stuck thread is left to finish and its result discarded). Concurrency: 2
probes at once.

> REVIEW: VE Core, 2026-09-25 (M4 A, 0.45.0): the flow is probe first, then add: files are probed in parallel on the pool and added in the order picked (`ImportQueue`), without pending cards before probing (decision (b)); the status bar counts progress. One import, however many files, is one undo step (`CompositeCommand`'s merge key). Before probing, a file that's a folder, can't be read or is empty is refused with that reason; the 20 s timeout is `ImportQueue`'s: the hung thread finishes on its own and its late result is dropped. The fingerprint (`core::fileFingerprint()`) is taken on the pool with the probe. Folders (Import Folder…, or dropped) are walked on the pool: every file under them, sorted, hidden files and folders skipped, at most 5,000. Failures are listed in a dialog, not only counted.

Image sequences (`%04d.png`) and stills: still images get a default length
of 5 s (setting), `isStillImage=true`, and the `pixbuf` producer with
`ttl`/`length` set by `EngineSync`.

## Master producers and cuts

`EngineSync` keeps `AssetId → std::shared_ptr<Mlt::Producer>` (the master).
Every timeline clip is `master.cut(in, out)`. Clip-level filters attach to the
cut, never the master. When an asset is relinked or its proxy toggled, the
master is replaced and every track using it is rebuilt.

MLT's avformat producer keeps one decoder context per producer *instance*;
cuts share the master's, which is what makes timeline playback efficient.
Two clips of the same asset back-to-back on one track can cause one decoder
to seek back and forth; kdenlive tolerates this and so do we in v2.0
(possible optimisation: a second master per asset for alternating use).

## Thumbnails (`engine/thumbnails.*`)

Worker with a small LRU of per-asset producers (its own, not the master,
because get_frame is not safe from two threads). Request: `(assetId,
frame, heightPx)` → `seek(frame)`, `get_frame()`,
`frame->set("rescale.interp", "nearest")`, `get_image(rgba, w, h)` with the
requested size so MLT's normaliser scales during decode. Result posted via the
dispatcher to `RenderCache`, which builds a `GdkMemoryTexture` and
`queue_draw`s the timeline. Disk cache: `$XDG_CACHE_HOME/ustudio/thumbs/<fingerprint-hash>/<frame>@<h>.rgba`
(raw; no PNG encode cost), capped at 512 MB LRU.

Bin cards use the frame at 10% of length; the timeline uses one per
interval, prioritised by visibility (the view sends the visible range; the
worker processes a deque with the most recently requested first).

## Waveforms (`engine/waveforms.*`)

One pass per asset at import (low priority, after probes): a private
producer, `get_audio(s16, 48000, ch, samples)` frame by frame, computing
peak (max abs) per 256 samples per channel, stored as `uint8_t` in
`$XDG_CACHE_HOME/ustudio/waves/<fingerprint-hash>.pk`. A 10-minute stereo
track is ~470 KB. The timeline downsamples further per zoom level at draw
time (max over a bucket). Progress is shown as the waveform filling in.

## Proxies (M4)

Optional per-asset proxy: 960px-wide H.264 (`avformat` consumer with
`vcodec=libx264 preset=veryfast crf=23 vf=scale=960:-2`), generated by
`u-studio-render --proxy`. Toggle "use proxies" swaps the master producer's
resource for the proxy path while keeping in/out (same frame count, ensured
by rendering at the sequence fps). Export always uses originals.

## Missing media and relink

On load, every asset's path is checked. Missing → `Status::Missing`; clips
render as a striped red placeholder and the engine substitutes a
`color:0x40000040` producer of the right length so the timeline still plays.
The relink dialog lists missing assets, lets the user pick a file or a folder
to search (matching by filename, then by fingerprint), and applies
`RelinkAsset` commands.

## Bin panel

`AdwViewStack` page in the left sidebar. `GtkGridView` of cards (thumbnail,
name, duration, resolution/fps badge, status). Folders as a flat path
selector for v2.0 (no tree). Drag source produces a `GdkContentProvider`
with our own MIME type `application/x-ustudio-asset` carrying the AssetId,
plus `text/uri-list` for convenience. Dropping on the timeline goes through
`InsertAt` with the drop position and the asset's full range; dropping a
file from Files on the bin or timeline triggers import first, then insert
once probing completes.
