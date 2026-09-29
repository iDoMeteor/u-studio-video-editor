# ADR-018: Clip transform is a core feature; the effects drop-in may animate it

**Status:** Accepted (owner, 2026-09-25, through the VE Strategist: "native
ability to move/resize/rotate clips... just like OBS handles image/video
scene items! ... i want this new feature in m4"). Amends doc 15, which put a
keyframed transform under the effects drop-in (FX).

## Context
Placing a picture in the frame (move, scale, rotate, crop, flip) is basic
editing for the owner's streams and promos, not an effect. With it in the
effects drop-in, a core-only install couldn't fit a 1344×768 capture into
a 1080p frame or put a webcam in a corner. And a picture of another size or
aspect needs *some* placement anyway: until 0.46.1 a smaller source drew
unscaled in the corner, and a 4:3 one still sits left.

## Decision
- **`Clip::transform` is core model data** (`core/model/types.h`), edited by
  core commands, saved by the core writer and played by `EngineSync`, with
  no drop-in present. Its UI (preview handles, the Edit Transform dialog) is
  core shell, built on the same IP5 preview-overlay host drop-ins use.
- **The model is OBS-like.** A bounds mode: Fit, the default, fits the
  cropped picture into the frame, centred; Stretch fills the frame; None
  places it explicitly. Explicit placement is a centre position and a size
  in project pixels, plus rotation in degrees about that centre. Crop is in
  source pixels per edge, and flip is horizontal or vertical. Project pixels
  make it independent of proxies and preview scale.
- **Every number is a `KeyframedValue`,** single-valued at first, so
  keyframing later is additive (the model and engine side landed
  2026-09-29: placement and rotation of an explicitly placed picture;
  crops stay single-valued; `docs/developer/notes/animation.md`). The effects drop-in may add a keyframing UI
  over these same fields (IP1 data, IP5 hosts), and doesn't own them.
- **The engine realises it with Qt-free MLT services per clip cut:** `crop`
  (core), `mirror` (core) and the `affine` filter (plus), onto a
  transparent background. Every track is composited onto track 0 by
  `composite` (fill=1). Measured in the F spike (2026-09-25): a chained
  compositor loses the upper clip's alpha, and an `affine` compositor
  (which would fit any aspect itself) costs four times as much per track.
  A default transform (Fit, no crop, flip or rotation) on a source of the
  frame's aspect adds no filters at all; another aspect gets the affine
  filter to fit and centre it.
- **Live edits don't rebuild.** A snapshot that differs only in transforms
  is applied to the existing filters in place, so a drag never restarts the
  consumer.

## Consequences
- Doc 15's "keyframed transform" becomes a keyframing UI on core data.
- The format gains the transform (version 6). Older projects load with
  the default Fit, which reproduces the 0.46.1 picture for same-aspect
  sources and centres the others.
- Transformed 1080p tracks cost CPU: about 16–22 ms per track per frame at
  full preview scale, and at 1080p Auto *is* Full. Three transformed tracks
  played at about 16 frames/s at Full and 25 at Half on the dev machine
  (F1, 2026-09-25). So Auto preview scale plays at Half once any clip has
  a non-default transform (0.47.2), which holds one transformed track at
  real time; more is MT4's (doc 19).

> REVIEW: VE Effects with VE Core, 2026-09-29: keyframed transforms (M5's
> gate). Keys live in `Transform`'s `KeyframedValue`s, relative to the
> clip's start: x, y, width, height and rotation; crops stay constant (MLT's
> `crop` isn't animatable) and a keyed placement is `Bounds::None`, both
> refused otherwise by `transformProblem()`. Core owns the model helpers
> (`transformAt()`, `withTransformAt()`, `placementAt()`), the animated
> `affine` rect and rotation in `transformFilters()` (every key scaled for
> preview and proxies; sampled only where x/y/w/h keys don't line up, then
> merged where linear), the writer's per-cut offsets, splitting, the
> key-aware preview handles and the in-place path; `movit.rect` animates on
> the GPU pending VE GPU, and a keyed rotation stays on the CPU chain. The
> effects drop-in owns the keyframing UI: a Transform card in the Rack
> (pins, previous/next, feel), transform curves in the curve lanes, and
> touch-record, all writing through `withTransformAt()`.
