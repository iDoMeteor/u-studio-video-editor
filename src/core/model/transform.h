#pragma once

// Clip transform arithmetic (ADR-018): where a clip's picture lands, in
// project pixels, for the engine (its affine rect) and the preview handles.
// Pure; no MLT.

#include "core/model/native_filter.h"
#include "core/model/types.h"

#include <string>
#include <utility>
#include <vector>

namespace ustudio::core {

// A clip's picture on screen: its centre, size and rotation (degrees
// clockwise about the centre), in project pixels.
struct Placement
{
    double cx = 0, cy = 0, w = 0, h = 0;
    double rotation = 0;

    bool operator==(const Placement &) const = default;
};

// The picture's size after crop, in source pixels (never below 1).
double croppedWidth(const Transform &t, int sourceWidth);
double croppedHeight(const Transform &t, int sourceHeight);

// Where `t` puts a picture of sourceWidth x sourceHeight (0: unknown, taken
// as the frame's own size) in `profile`'s frame.
Placement placementFor(const Transform &t, int sourceWidth, int sourceHeight, const Profile &profile);

// The same placement as explicit None bounds: what the handles switch to
// when a Fit or Stretch picture is first moved or scaled.
Transform explicitTransform(const Transform &t, int sourceWidth, int sourceHeight, const Profile &profile);

// True when `t` needs no filters: Fit or Stretch of a source of the frame's
// aspect, uncropped, unflipped, unrotated. The track compositor (composite,
// fill=1) scales such a picture to the frame by itself.
bool isIdentity(const Transform &t, int sourceWidth, int sourceHeight, const Profile &profile);

// True when the track compositor (composite, fill=1, centred) places `t`'s
// picture by itself whatever its aspect: a plain Fit, uncropped, unflipped,
// unrotated. EngineSync and the writer then skip transformFilters() for a
// clip's own cuts (the affine filter costs a frame-sized canvas, 16-18 ms at
// 1080p), but not for a dissolve's cuts: luma mixes both at one size, so a
// picture of another aspect there must arrive already frame-sized.
bool compositorFits(const Transform &t);

// True when a clip of the active sequence has a transform other than the
// default Fit: the engine then plays Auto preview scale at Half (each
// transformed 1080p track costs 16-22 ms a frame at Full; doc 19, MT4). A
// default Fit of another aspect has a filter too but doesn't count: it is
// the picture as imported, one track at most in the common case.
bool hasTransformedClip(const Project &project);

// The MLT filters that realise `t` on a clip's cut, in order: crop (core),
// mirror (core; "flip" is horizontal, "flop" vertical), affine (plus: the
// picture placed on a transparent frame; rotation about its centre). Empty
// for an identity transform. Shared by EngineSync and the writer's render
// graph, so a saved project plays in melt as in the editor. `outputScale`
// is output pixels per project pixel (a scaled preview); `sourceScale` is
// the playing file's pixels per source pixel (a proxy is smaller).
std::vector<NativeFilter> transformFilters(const Transform &t, int sourceWidth, int sourceHeight,
                                           const Profile &profile, double outputScale = 1.0, double sourceScale = 1.0);

// The same transform for the GPU pipeline (ADR-019 point 5): the mirrors
// become movit.mirror (horizontal) and movit.flip (vertical), and the
// placement movit.rect, since no movit service rotates. A rotated transform
// keeps transformFilters()' CPU chain whole (one CPU island in the GPU
// graph, not two). Crop stays the core `crop` filter.
std::vector<NativeFilter> gpuTransformFilters(const Transform &t, int sourceWidth, int sourceHeight,
                                              const Profile &profile, double outputScale = 1.0,
                                              double sourceScale = 1.0);

// Transform's check() rules: sizes positive when placed explicitly,
// crops non-negative, every value finite. "" when fine.
std::string transformProblem(const Transform &t);

} // namespace ustudio::core
