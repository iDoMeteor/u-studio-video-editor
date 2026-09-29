#pragma once

// The Rack's Transform card (ADR-018, keyframed clip transforms): what a
// change to one of a clip's transform values does to its Transform. Keys
// count from the clip's first frame; a keyed placement is explicit
// (Bounds::None, transformProblem()), so edits that key a value start from
// the placement the picture shows (explicitTransform()). Pure.

#include "core/model/types.h"

#include <vector>

namespace ustudio::effects {

// The values a clip's transform may animate (crops stay constant).
enum class TransformField
{
    X,
    Y,
    Width,
    Height,
    Rotation,
};

const core::KeyframedValue &fieldOf(const core::Transform &t, TransformField field);
core::KeyframedValue &fieldOf(core::Transform &t, TransformField field);

// `field` set to `value` by hand `at` frames into the clip. `placed` is `t`
// as an explicit placement (t itself when already one). An animated
// transform gets the key at `at` for a keyed value and the new value for a
// static one (withTransformAt()); otherwise the value itself, on `placed`,
// except that rotation alone leaves a Fit or Stretch picture as it is.
core::Transform withValue(const core::Transform &t, const core::Transform &placed, TransformField field,
                          core::FrameIndex at, double value);

// `field`'s keys replaced by `keys` on `placed` (a pin, a feel, touch-
// record); without keys, `shown` becomes its value.
core::Transform withKeys(core::Transform placed, TransformField field, std::vector<core::Keyframe> keys, double shown);

} // namespace ustudio::effects
