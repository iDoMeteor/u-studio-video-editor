# 08 — Effects, keyframes, compositing, transitions

> **Superseded in large part (2026-09-23)** by
> [doc 15](15-effects-and-transitions.md) and
> [ADR-011](adr/011-frei0r-required-and-effect-families.md): frei0r is now
> required, the curated-only catalogue becomes a generated registry with
> curated overlays, and the effect panel becomes the Effect Rack and
> Browser. Doc 15's first table lists which sections below still hold.
> Titles moved to [doc 16](16-titles-tool.md).

## Vocabulary

- **Effect**: an MLT *filter* attached to one clip cut or one track playlist
  (`Effect` in the model, doc 03).
- **Compositing**: how a video track blends over the tracks below. A
  per-track-pair MLT *transition* that is always active; not a model object
  the user edits (only opacity/transform effects on clips are).
- **Transition** (user-facing): a dissolve between two adjacent clips on one
  track (`Transition` in the model).

## Compositing without Qt or frei0r (ADR-006)

Available on this machine: `composite`, `affine`, `luma`, `mix`, `matte`,
`movit.*`. `frei0r.cairoblend` is absent (package not installed) and
`qtblend` is excluded by policy. Plan:

- **Track blending**: `composite` transition between each video track and
  the composite of everything below, `always_active=1`, `fill=1`, `aligned=0`,
  `progressive=1`, geometry `0/0:100%x100%`. `composite` honours the upper
  frame's alpha, so opacity and transforms on clips "just work".
- **Per-clip transform + opacity**: the `affine` *filter* on the clip
  (`transition.rect` keyframable geometry `x y w h opacity`, plus
  `transition.fix_rotate_x`). Exposed as the "Transform" effect:
  position, size, rotation, opacity, with keyframes.
- **If frei0r is present at runtime**, offer `frei0r.cairoblend` as the track
  compositor (better quality, blend modes) via a preference; detection is a
  `Repository` query. Never required.
- Audio mixing: `mix` transition per audio-bearing track, `always_active=1,
  sum=1`, `a_track = below, b_track = track`.

Kdenlive's `mlt/modules` usage for these is the reference:
`~/Repos/kdenlive/src/timeline2/model/timelinemodel.cpp` (search
`"composite"`, `"mix"`).

## Effect catalogue

Not "all 526 filters". A curated list with metadata we author
(`data/effects/*.json`), each mapping to one MLT service and a UI schema:

| UI name | MLT service | Params (type) | Notes |
|---------|-------------|---------------|-------|
| Transform | `affine` (filter) | rect (x,y,w,h,opacity) keyframable, rotation | doc above |
| Opacity | `brightness` with `alpha` | 0–1 keyframable | cheaper than affine when only opacity is needed |
| Crop | `crop` | left/right/top/bottom px, `center` | |
| Brightness/Contrast/Saturation | `avfilter.eq` | brightness, contrast, saturation, gamma | |
| Lift/Gamma/Gain | `lift_gamma_gain` | 3× colour | |
| Blur | `avfilter.gblur` | sigma keyframable | |
| Volume | `volume` | gain dB keyframable | |
| Fade in/out (video) | `brightness` keyframes on `level` | sugar | model `FadeSpec` |
| Fade in/out (audio) | `volume` keyframes | sugar | |
| Normalise | `avfilter.loudnorm` | preset | offline-ish; warn |
| Denoise (audio) | `rnnoise` | strength | present |
| Text/Title | `dynamictext` filter or `pango` producer clip | text, font, size, colour, position | Qt-free text |
| Speed | producer `timewarp:` | ratio | M6+; changes length, so it is a *clip* property not an effect |
| Stabilise | `vidstab` | preset | needs analysis pass; M6+ |

The catalogue JSON drives the effect panel generically: each param type
(`double` slider with range, `int`, `bool`, `color`, `rect`, `enum`, `text`,
`font`) has one widget. Custom UIs only for Transform (on-preview handles)
and Text.

Metadata self-check at startup in debug builds: for each catalogue entry,
`repository->metadata(mlt_service_filter_type, service)` must be non-null and
every param name must exist in its `parameters` list. Missing services hide
the entry at runtime with a log line rather than crashing.

## Keyframes

`Param::keyframes` serialise to MLT's animation string format
(`"0=50;100~=75;200|=10"` where `=` linear, `~` smooth, `|` hold), applied via
`Mlt::Properties::set(name, string)`. For `rect` params the value format is
`x y w h opacity`. `EngineSync` regenerates the string on every
`EffectChanged`. Keyframe positions are relative to the clip's in point,
which is how MLT interprets animation on a filter attached to a cut.

UI: a keyframe lane under the clip in the timeline (toggle per effect) and a
keyframe toggle next to each keyframable param in the effect panel; the
value editor writes a keyframe at the playhead when keyframing is on for that
param, otherwise sets the constant.

## Transitions (dissolves) between clips

Model: `Transition{track, a, b, length}` where `a` ends where `b` starts (or
they overlap by `length` after the command extends the tails). Command
`AddTransition` requires enough handles on both clips (a has `out+length-1`
available, b has `in-length` ≥ 0); otherwise refused with a message.

Engine: per kdenlive's "mix" approach, the overlap region on the playlist is
replaced by a 2-track `Mlt::Tractor` (a's tail cut on track 0, b's head cut
on track 1) with a `luma` transition (`reverse`/`softness` from the model) and
a `mix` for audio. The verifier treats this sub-tractor as one entry with the
combined length. Rebuilding a track re-creates these; nothing incremental.

Only "dissolve" (`luma` with no wipe file) and "dip to black" (two fades) in
v2.0. Wipes with luma images are an easy follow-up (`luma` with `resource`).

## Effect panel

Right sidebar page. Shows the effect stack for the current selection (clip,
else track, else nothing). Rows: enabled switch, name, drag handle to
reorder, reset, remove. Expander reveals params. Add via a searchable popover
over the catalogue. All changes go through commands (`SetParam` merged
during slider drags).

## Preview-side manipulators (M5)

For Transform: a `GtkOverlay` on the preview draws the clip rect with corner
handles; dragging emits `SetParam` on the `affine` rect at the playhead (as a
keyframe if keyframing is on). Coordinates map from widget space to profile
pixels via the preview's content-fit rectangle.
