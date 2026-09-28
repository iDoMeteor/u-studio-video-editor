# Keyframes and easing

[Docs home](../../README.md) › [Developer docs](../README.md) › [Implementation notes](README.md) › Keyframes and easing

The project has **one** keyframe model, shared by effects (doc 15) and
titles (doc 16). It lives in `src/core/model/`; neither drop-in defines its
own keyframe or easing type.

## The model

| Type or function | Where | What it is |
|---|---|---|
| `core::Easing` | `types.h` | MLT's `mlt_keyframe_type`, value for value (pinned by `static_assert`s in `engine_sync.cpp`) |
| `core::Keyframe` | `types.h` | `{at, value, easing}`: `at` is frames from the owner's start, `easing` shapes the segment *to the next* keyframe |
| `core::KeyframedValue` | `types.h` | A number that may be animated: `value` when `keyframes` is empty |
| `core::easedValue(keyframes, frame)` | `animation.h` | The value at `frame` (a `double`), exactly as MLT computes it |
| `core::animationString(keyframes)` | `animation.h` | MLT's animation string, `"0=0;30g=1"`, for filters that MLT animates itself |
| `core::keyframesForCut(keyframes, offset, length)` | `animation.h` | One cut's keyframes: shifted by `-offset`, with an edge keyframe at each end whose value is `easedValue()` there |
| `core::easingOperator()` / `easingFromOperator()` | `animation.h` | The operator character between a keyframe's position and `=` |
| `core::easingName()` / `easingFromName()` | `animation.h` | Stable lowercase names (`cubic_out`, `smooth_natural`) for files and UI; the titles format writes these |

Two ways to use it:

- **MLT animates** (effects): write `animationString(keyframesForCut(...))`
  into the filter property, and MLT interpolates on the consumer thread.
- **We animate** (titles, and any other in-process evaluation): call
  `easedValue()` per frame. A title keyframe and an effect keyframe with the
  same easing therefore move identically.

Colours are animated per channel: a drop-in keeps one `std::vector<Keyframe>`
per channel, as MLT does inside `interpolate_item()` for colour properties.

## How MLT interpolates (and so `easedValue()` does)

From `mlt_animation.c` in MLT 7.40 (`mlt_animation_get_item()`,
`interpolate_value()`, the `*_interpolate()` helpers), reproduced in
`src/core/model/animation.cpp`:

- A segment between two keyframes eases with the **first** keyframe's type.
- Before the first keyframe and after the last, the value **holds** at that
  keyframe. There is no extrapolation.
- `Discrete` holds the first value until the next keyframe's frame, then jumps.
- The three smooth kinds are Catmull-Rom over four points: the keyframe
  before the segment, its two ends, and the one after. At either end of the
  list the missing point duplicates the end, which MLT then moves 10,000
  frames away, so the curve arrives flat there. Loose is `alpha 0,
  tension 1` (it may overshoot). Natural is `alpha 0.5, tension -1` (a flat
  tangent at peaks, so no overshoot). Tight is `alpha 0.5, tension 0` (flat
  at every keyframe).
- The rest are Robert Penner's curves, applied as a fraction of the
  segment's change. Back, elastic and bounce leave `[0, 1]`, so a value can
  overshoot its keyframes.

`easedValue()` accepts fractional frames (MLT's positions are whole). Titles
need this for per-character clocks. At whole frames it agrees with
`Mlt::Properties::anim_get_double()` to 1e-9 for every easing
(`tests/engine/test_eased_value.cpp`, test `engine-eased-value`).

## Cut edges

A cut that starts or ends inside a segment gets an edge keyframe whose
value is the exact eased value there. The edge keeps its segment's
easing, which **restarts** at the edge, so between the edge and the next
keyframe the shape is close to the original, not identical. That is visible
only for strongly shaped easings on a short cut. The fix, if it is ever
needed, is to sample the segment densely into linear keyframes.
