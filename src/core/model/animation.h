#pragma once

#include "core/model/types.h"

#include <string>
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

// The keyframes for one cut of an owner, as that cut's own animation: the
// cut starts `offset` frames into the owner and lasts `length`. Keyframes
// are shifted by -offset; any outside [0, length) are replaced by a
// keyframe on the cut's edge carrying the value interpolated there
// (linearly: the exact eased value is the engine's to compute, IP3), so the
// animation still passes through the right values at the cut's edges.
std::vector<Keyframe> keyframesForCut(const std::vector<Keyframe> &keyframes, FrameIndex offset, FrameIndex length);

// "0=0;30g=1": MLT's animation string (positions relative to the cut).
std::string animationString(const std::vector<Keyframe> &keyframes);

// Formats a double the way the project format does (locale-independent,
// shortest round-trip).
std::string formatDouble(double value);

} // namespace ustudio::core
