#pragma once

// What MLT runs for one effect on one cut (doc 15, "Engine mapping" and
// "Mix and masks"): the one place that turns a core::Effect into MLT filter
// properties, so the project file (core/xml, for melt) and the engine's
// effects drop-in (IP3, for preview and export) build the same filters.
//
// An effect at a constant full mix is its own filter. Any other mix wraps it
// in MLT's mask_start / mask_apply pair: mask_start snapshots the frame and
// runs the effect (its "filter." properties are the effect's), and
// mask_apply composites the result back over the snapshot through a
// transition at the mix's opacity. mask_apply's default transition is
// qtblend, which ADR-007 denies, so the transition is always named.
// Masks (EffectMask) are FX2's and not realised yet: the model keeps them.

#include "core/model/native_filter.h"
#include "core/model/types.h"

#include <string>
#include <utility>
#include <vector>

namespace ustudio::core {

// mask_apply's transition for the mix. frei0r.cairoblend's "0" is its
// opacity: 50% brightness mix of grey 128 at level 2 gave 192, for 6 ms a
// 1080p frame over the plain effect; the plus module's affine ("rect"'s
// fifth value, a percentage) gave 191 for 14 ms; composite ignored its
// opacity there (255) and cost 32 ms (standalone repros, MLT 7.40, frei0r
// 2.5.6, 2026-09-28; docs/developer/notes/effects.md). The effects drop-in
// ships with frei0r (ADR-014), so cairoblend is the default; affine is the
// fallback when frei0r isn't there.
enum class MixTransition
{
    Cairoblend,
    Affine,
};
const char *mixTransitionService(MixTransition transition);

// Whether `effect` needs the mask_start / mask_apply pair: a mix that isn't
// a constant 1.
bool needsMixWrap(const Effect &effect);

// MLT's form of a constant value (0xRRGGBBAA colours, "x y w h" rects).
std::string nativeValue(const Param::Value &value);
// A parameter on a cut `offset` frames into its owner and `length` long: an
// animation string (keyframesForCut()) when keyframed, else nativeValue().
std::string nativeParam(const Param &param, FrameIndex offset, FrameIndex length);

// The filters for `effect` on that cut, in attach order: the effect alone;
// or mask_start, mask_apply (a constant mix); or mask_start, brightness
// (the keyframed mix as its alpha), mask_apply. "disable" is set on each
// when the effect is off. In/out are the caller's (engine::attachToCut(),
// the writer's cutIn).
std::vector<NativeFilter> nativeFilters(const Effect &effect, FrameIndex offset, FrameIndex length,
                                        MixTransition mix = MixTransition::Cairoblend);

} // namespace ustudio::core
