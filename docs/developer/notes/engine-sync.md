# Engine sync notes

[Developer docs](../README.md) › [Implementation notes](README.md)

**mlt++ accessors that return a pointer allocate a new wrapper — delete
it, whatever the header says.** `Mlt::Tractor::field()` and
`Mlt::Tractor::track(int)` are documented "caller does not own the
result", but each call returns a fresh wrapper holding its own reference on
the underlying MLT object. Calling `field()` inline without deleting it
leaked every transition planted in every rebuilt tractor, ~100–330 KB per
edit (sanitizer report S1, 2026-09-23; confirmed with
`docs/audit/2026-09-23-sanitizer-run/rebuildrepro.cpp`: RSS +10 MB per 100
rebuilds leaked, flat when deleted, and ASan-clean either way). Hold these
in a `std::unique_ptr`.

**Building a playlist entry by entry is O(n²).**
`mlt_playlist_append()` and `blank()` end in
`mlt_playlist_virtual_refresh()`, which walks every entry doing three
locked property lookups each (`mlt_playlist.c`, 7.40). With 5,000 clips on
one track a rebuild took 17–21 s. MLT has no batch or deferred-refresh
API. So `EngineSync::rebuildTrackPlaylist()` builds a track with more than
64 entries from nested sub-playlists of 64, each marked `ustudio.chunk`.
That's linear, and frame-, audio- and render-identical to the flat graph
(`engine-chunked-playlist`). `verify()` flattens the chunks back. A track
of 64 entries or fewer stays flat, exactly as before (doc 19, MT2 piece 0).

**Every `mix` transition is a 9.2 MB `calloc()`, so pin glibc's mmap
threshold.** `transition_mix.c` embeds two 192,000-sample × 6-channel
float buffers. There is one per track and one per dissolve. glibc
serves them from fresh, already-zero mmap pages until its dynamic mmap
threshold climbs past 9.2 MB. It does that the first time one is freed,
i.e. on the first rebuild. After that each one comes from the heap and
is zeroed in full. On a 5,000-clip project with 1,992 dissolves, rebuilds
went from 0.35 s to 5.7 s and resident memory reached 36 GB within nine
rebuilds. `FactoryPolicy` sets `M_MMAP_THRESHOLD` to 4 MiB before
`Factory::init()`, which also disables the dynamic adjustment. Rebuilds
then stay at 0.3 s and 400 MB. `engine-sync`'s "resident memory" test
fails without it (+5.4 GB).

`EngineSync` rebuilds a track's whole MLT playlist from the model on any
change (clear it, re-append blanks and cuts in position order) rather than
doing incremental playlist surgery (ADR-005) — simpler and always
consistent by construction, at the cost of being O(clips on the track) per
edit instead of O(1); acceptable at today's scale per
[doc 13's risk R5](../../plans/v2/13-risks-and-open-questions.md). Cuts share
a per-asset master producer (`Mlt::Producer::cut()`), so no source file is
reopened per clip. `playlist.get_clip()`'s own `resource` property on a cut
is the placeholder string `"<producer>"`, not the real file path — that's
on the master producer / the model's own `Asset::path`, confirmed
empirically with a standalone repro. A clip's `videoEnabled`/
`audioEnabled` flags (used by "Split Audio") are applied through a
separate master producer per (asset, video on/off, audio on/off)
combination, with `video_index`/`audio_index` set to `-1` ("off", per
`avformat`'s own YAML) on that master. **Not on the cut:** MLT ignores both
properties on a cut, confirmed with a standalone repro (2026-09-24). A cut
with `audio_index=-1` still played at full level, while the same property
on its master silenced every cut taken from it. Until then, a Split Audio
clip's video half kept playing its sound underneath the new audio clip.
Each clip's own variant means silencing one clip never affects another
cut of the same asset. Per-track volume
(`Track::volume`, a linear 0..1 scale for the UI) is applied by attaching
an `Mlt::Filter("volume")` to the track's playlist and converting to the
filter's own `"level"` property (dB — `"gain"` is documented deprecated
in its YAML metadata) with the standard `20*log10` amplitude-ratio
formula; confirmed with a standalone repro that -20dB measures a 0.1x
peak-amplitude ratio, exactly as expected, and again against the real
engine pipeline in `tests/engine/test_engine_sync.cpp`.

A dissolve transition between two adjacent same-track clips
(`core::Transition`, `AddTransition`/`RemoveTransition`) is built as a
small 2-track `Mlt::Tractor` (clip `a`'s tail on track 0, clip `b`'s head
on track 1, connected by `luma` + `mix`) nested as one entry inside the
track's own playlist — `core::planTrackSegments()` (shared with the XML writer) is the single
source of truth both `rebuildTrackPlaylist()` and `verify()` build/check
against, so they can't drift. Both the `luma` and `mix` transitions
**must** have their own `in`/`out` set explicitly to the sub-tractor's
local `[0, length)` range — confirmed empirically (2026-09-22) that
leaving them unset corrupts the video dissolve into flat garbage colour
for the last couple of overlap frames, but *only* once track 0's cut
producer has the non-zero absolute `in` a real clip's tail always has (a
toy zero-based repro is not enough to catch this). `mix` additionally
needs `start=-1` ("automatic linear crossfade", per its own YAML) rather
than the `sum=1, always_active=1` config used for the permanent
cross-track audio blend (see [Playback engine notes](playback-engine.md)) — the YAML documents `sum` as incompatible
with `start < 0`, confirming the two uses need different settings; the
crossfade's audio correctness rests on that documented semantics plus the
same explicit-in/out fix, not an independent sample-level measurement (an
RMS probe on two `tone:` generators wasn't discriminating enough either
way). See `EngineSync::buildTransitionSubTractor()`'s comment and
`tests/engine/test_engine_sync.cpp`'s dissolve test (pixel-samples the
actual composited output) for the full finding.

A `core::Transition` assumes its two clips keep exactly the geometry
`AddTransition` gave them; nothing else used to know it existed. A
2026-09-22 audit found that removing, moving, resizing, or splitting
either linked clip left the transition dangling or pointing at the wrong
span — a crash on the next right-click/drag on that row (an unguarded
`m_model.clip(t.a)`/`clip(t.b)` in the timeline's right-click and
drag-begin handlers), and a saved project that fails `loadProject()`'s
own `check()` refusal on reopen. `RemoveClip`, `MoveClip`, `ResizeClip`,
`SplitClip`, and `RemoveTrack` each strip a transition touching the
clip(s) they're about to change first (`Model::removeTransition`, which
un-extends both linked clips back to pre-dissolve geometry) and restore
it on revert (`Model::addTransition`) — the clip simply loses its
dissolve, the same outcome a manual "Remove Transition" then the edit
would have produced. `SplitAudio` is deliberately untouched: it only
toggles a clip's video/audio-enabled flags, never its position/in/out, so
a transition on it stays geometrically valid throughout. The same audit
found a second, narrower bug (a clip linked on both sides at once — the
middle of an A-dissolve-B-dissolve-C chain — can have combined overlap
longer than its own length, which `AddTransition`'s single-transition
check doesn't catch but corrupts `planTrackSegments`'s layout); both
`AddTransition` and `Model::check()` now enforce it.

A 2026-09-23 follow-up audit found the T1 fix above was too eager:
`ResizeClip`/`SplitClip` stripped (or flatly refused) an edit touching
only a clip's *untouched* edge — trimming a clip's far tail away from an
incoming dissolve on its head, for instance, was refused outright,
because `Model::isRangeFree`'s overlap check only ever ignored the clip
being edited, not the still-present, still-legitimately-overlapping
partner clip a transition it wasn't stripping left in place.
`isRangeFree` now takes a set of ids to ignore (the clip itself, plus the
partner of any transition this edit determined it doesn't need to strip),
and `ResizeClip`/`SplitClip` strip only the transition(s) whose own
overlap region the edit would actually reach into — everything else on
the clip keeps its dissolve. `SplitClip` additionally repoints a
surviving *outgoing* transition from the original clip onto the new right
half (`Model::retargetTransitionClip`, which changes a transition's `a`/
`b` without touching either clip's geometry) rather than leaving it
attached to the half that no longer owns that edge. Two narrower bugs
from the same audit: `MoveClip::apply()` now refuses outright — before
touching the model at all — a "move" whose destination track and
position exactly match the clip's current ones (a click, or a small
same-row wobble the drag-vs-click pixel threshold didn't fully absorb),
since issuing it anyway would strip a dissolve for an edit that changes
nothing; and `RemoveAsset` now strips each of its clips' transitions
*before* capturing their (now pre-dissolve) geometry for revert, in the
same batch as the removal, so deleting an asset no longer leaves a
dangling transition on a clip about to disappear or on a surviving clip
it was linked to.

Fixing T1 surfaced a second, unrelated crash: `RemoveClip`/`MoveClip`/
`RemoveAsset` (and, before this fix, the un-batched T1 strip-then-edit
sequence) each performed two or more separate `Model` mutations with no
`BatchBegin`/`BatchEnd` around them, so `EngineSync` ran a full consumer
stop/reselect/restart *per mutation* instead of once. Confirmed via
`coredumpctl` + `gdb` (2026-09-23, a real crash hit live testing this
exact fix): two such restarts back to back segfaults deep inside
PipeWire/SDL3's own stream teardown (`pw_stream_destroy` →
`unref_plugin` → `dlclose`), unrelated to anything this app controls —
purely a consequence of tearing the real audio device down and rebuilding
it twice in immediate succession. Every multi-mutation command in
`core/commands/primitives.cpp` now wraps its whole apply()/revert() in
one `BatchBegin`/`BatchEnd` pair (matching the pattern `InsertClip`/
`ResizeClip`/`CompositeCommand` already used), so `EngineSync` always
coalesces to exactly one rebuild per command, however many `Model` calls
it makes internally.

`masterProducerFor()` checks `Mlt::Producer::is_valid()` right after
opening an asset's file and, on failure, substitutes a `color:black`
placeholder (sized to the asset's own recorded length) instead of caching
the broken producer — confirmed empirically (a standalone repro,
2026-09-23) that an **invalid producer still lets `.cut()` "succeed"**:
the resulting cut reports `is_valid()==true`, appends to a playlist with
no error, and the tractor built from it reports a normal length — it only
segfaults once a real frame is pulled through the live consumer, deep
inside MLT's own `mlt_producer_seek`/`transition_get_frame`. That means
open time is the *only* place this can be caught; by the time a bad
producer would otherwise reach the playlist, it's indistinguishable from
a real one. `EngineSync::mediaUnavailable` fires (main thread, from
`rebuildAll()`) so the app layer can tell the user which file is missing;
`verify()` skips its resource-match check for these clips, since the
placeholder's resource is the intended fallback, not a sync bug.

**A filter on a cut animates from the cut's `in`, only if you set it.**
A cut's frames carry their position in the *source*, and
`mlt_filter_get_position()` returns that position minus the filter's own
`in` (`mlt_filter.c`, 7.40). A filter attached to a cut of source frames
100–194 with the default in 0 read its animation from frame 100 onwards,
so a fade keyed 0→94 started most of the way through. Give the filter the
cut's in/out: `engine::attachToCut()` does, for drop-in extensions (IP3),
and the project writer puts the same `in`/`out` on a clip entry's native
`<filter>`. Confirmed with a standalone repro and
`tests/dropins/test_dropin_engine` (2026-09-25).

**Tracks composite with `composite` (fill=1) onto track 0, and a clip's
transform is an `affine` filter on its cut (ADR-018, M4 F).** Each finding
from a standalone repro (MLT 7.40, 2026-09-25):

- `composite`'s `fill` defaults to 0 whatever its YAML says (the code reads
  it with `mlt_properties_get_int()`), so a picture smaller than the
  project drew at its own size in the top-left corner. With `fill=1` a
  picture of the frame's aspect fills it, but another aspect (4:3, 1344×768)
  sits at the left, and `halign`/`valign` with `fill` offset by the unscaled
  size. So a clip of another aspect gets the affine filter below even when
  its transform is the default Fit (`core::isIdentity()`).
- **The `affine` transition is too slow to be the track compositor.** It
  fits and centres any aspect by itself, but cost 21 ms a frame for one
  untransformed 1080p track (39 ms for four) against `composite`'s 5 (7).
- **A chained compositor loses the upper clip's alpha.** With V1→V2 and
  V2→V3 chained, a transformed V3 picture showed black, not V1, around it.
  Every track's compositor takes track 0 (the background) as its A track
  (`plant_transition(composite, 0, index)`), bottom track first. The audio
  `mix` stays chained (`index - 1, index`).
- **The transform filter** (after `crop` and `mirror`) needs
  `use_normalized=1` (the canvas is the profile's size with the source
  stretched to it, so `transition.rect` is where the picture lands; without
  it the canvas is the source's size), `transition.repeat_off=1` and
  `transition.mirror_off=1` (else it tiles and mirrors outside the rect),
  and `transition.distort=1`. Rotation is `transition.fix_rotate_x`, in
  degrees about the rect's centre. It costs about 16–22 ms a frame per
  1080p track (the affine interpolation, already sliced across cores, plus
  YUV→RGBA→YUV conversion); see doc 19, MT4.
- With `use_normalized` the filter returns the profile's size *whatever
  size is asked for*: a `luma` asked for a smaller frame then gets two
  sizes and doesn't mix. Consumers ask for the profile's size, so playback
  and render dissolve correctly; a test pulling frames directly must too
  (`tests/engine/test_transform`).
- **Each affine filter keeps a frame-sized image for its whole life**: it
  makes its own `colour:0` background producer, and `producer_colour`
  caches its last image (`filter_affine.c`, `producer_colour.c`). At 1080p
  that is 8 MB per transformed cut that has played: RSS rose 438 → 750 MB
  in a 2-minute soak. `EngineSync::applyTransform()` gives every filter one
  shared background (a reference each); RSS then stayed flat. Projects
  played directly in `melt` still keep one per filter.
- A frame pulled bare interpolates rotated edges differently from the
  consumers (`rescale=bilinear`); set `consumer.rescale` on it to compare
  with a render byte for byte.

**Image sequences play through `pixbuf` with an explicit `begin`, `ttl=1`
and a counted length (M4 E, 0.51.0-beta.1).** `pixbuf:<dir>/frame_%04d.png?begin=N`
plays one picture per frame with `ttl=1` (the default is 25 each); its own
length is a default 15,000 and it loops after the last file, so
`EngineSync::masterProducerFor()` sets the length it counted at import.
`?begin=` works only with the explicit `pixbuf:` prefix (through the default
loader the producer is invalid). `avformat`'s image2 path opens a sequence
but returns the last picture for every frame, so there's no fallback where
gdk-pixbuf can't load images. `verify()` accepts the `?begin=` suffix on a
sequence's resource. A still's size is now read at probe by decoding one
frame (`meta.media.width`/`height` appear on the first `get_image`).
Standalone repro, MLT 7.40, and `tests/engine/test_image_sequence`.
