#pragma once

// Keyframe editing for the Rack (doc 15, "Keyframes that feel musical"):
// pins, previous/next, and the named feels. Pure functions over the shared
// keyframe model (core::Keyframe, evaluated by core::easedValue()); frames
// are relative to the owner (a clip's first frame, else the sequence's).

#include "core/model/types.h"

#include <optional>
#include <string>
#include <vector>

namespace ustudio::effects {

// The key at `at`, if any.
const core::Keyframe *keyAt(const std::vector<core::Keyframe> &keys, core::FrameIndex at);
// `keys` with the value at `at` set: that key's value changed, or a new key
// (with `easing`) inserted in order.
std::vector<core::Keyframe> withKeyAt(std::vector<core::Keyframe> keys, core::FrameIndex at, double value,
                                      core::Easing easing = core::Easing::Linear);
std::vector<core::Keyframe> withoutKeyAt(std::vector<core::Keyframe> keys, core::FrameIndex at);
std::vector<core::Keyframe> withEasingAt(std::vector<core::Keyframe> keys, core::FrameIndex at, core::Easing easing);
// The nearest key strictly before / after `at`.
std::optional<core::FrameIndex> previousKey(const std::vector<core::Keyframe> &keys, core::FrameIndex at);
std::optional<core::FrameIndex> nextKey(const std::vector<core::Keyframe> &keys, core::FrameIndex at);

// The feel chips: a name for each easing people reach for; "All curves"
// offers the rest. The easing is the curve from this key to the next.
struct Feel
{
    const char *name;
    core::Easing easing;
};
const std::vector<Feel> &feels();
// The feel's name for an easing, or the easing's own name ("quartic_in").
std::string feelName(core::Easing easing);

} // namespace ustudio::effects
