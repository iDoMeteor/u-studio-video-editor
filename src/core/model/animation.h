#pragma once

#include "core/model/types.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ustudio::core {

// MLT's animation-string operator for an easing: the character written
// between a keyframe's position and '=' (mlt_animation.c's
// keyframe_type_map, MLT 7.40: "" linear, "|" discrete, "~" smooth, "$"
// natural, "-" tight, then 'a'...'z', 'A'...'D' for sinusoidal in ... bounce
// in-out).
const char *easingOperator(Easing easing);
// The inverse; unknown characters (and none) are linear.
Easing easingFromOperator(char op);

// A stable lowercase name for files and UI ("cubic_out", "smooth_natural"),
// and the inverse (nullopt for an unknown name). The titles format writes
// these; MLT's own strings use easingOperator().
const char *easingName(Easing easing);
std::optional<Easing> easingFromName(std::string_view name);

// The keyframes for one cut of an owner, as that cut's own animation: the
// cut starts `offset` frames into the owner and lasts `length`. Keyframes
// are shifted by -offset; any outside [0, length) are replaced by a
// keyframe on the cut's edge carrying the value there (easedValue(), exact),
// so the animation passes through the right values at the cut's edges. The
// edge keeps its segment's easing, which restarts from the edge: between an
// edge and its neighbour the curve's shape is close, not exact.
std::vector<Keyframe> keyframesForCut(const std::vector<Keyframe> &keyframes, FrameIndex offset, FrameIndex length);

// The value of `keyframes` (sorted by `at`) at `frame`, computed the way
// MLT's mlt_animation_get_item() / interpolate_value() does (mlt_animation.c,
// MLT 7.40): each segment eases with its *first* keyframe's easing, the
// smooth kinds take the neighbouring keyframes as Catmull-Rom control points
// (duplicating the ends), and the value holds at the first or last keyframe
// outside them. `frame` may be fractional (MLT's is always whole), for
// titles' per-unit clocks. Empty: 0. Pinned against Mlt::Animation for every
// easing by tests/engine/test_eased_value.cpp.
double easedValue(const std::vector<Keyframe> &keyframes, double frame);

// "0=0;30g=1": MLT's animation string (positions relative to the cut).
std::string animationString(const std::vector<Keyframe> &keyframes);

// Formats a double the way the project format does (locale-independent,
// shortest round-trip).
std::string formatDouble(double value);

} // namespace ustudio::core
