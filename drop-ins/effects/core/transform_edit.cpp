#include "core/transform_edit.h"

#include "core/model/transform.h"

namespace ustudio::effects {

const core::KeyframedValue &fieldOf(const core::Transform &t, TransformField field)
{
    switch (field) {
    case TransformField::X:
        return t.x;
    case TransformField::Y:
        return t.y;
    case TransformField::Width:
        return t.width;
    case TransformField::Height:
        return t.height;
    case TransformField::Rotation:
        break;
    }
    return t.rotation;
}

core::KeyframedValue &fieldOf(core::Transform &t, TransformField field)
{
    return const_cast<core::KeyframedValue &>(fieldOf(static_cast<const core::Transform &>(t), field));
}

core::Transform withValue(const core::Transform &t, const core::Transform &placed, TransformField field,
                          core::FrameIndex at, double value)
{
    if (core::isAnimated(t)) {
        core::Transform shown = core::transformAt(t, at);
        fieldOf(shown, field).value = value;
        return core::withTransformAt(t, at, shown);
    }
    core::Transform next = field == TransformField::Rotation ? t : placed;
    fieldOf(next, field).value = value;
    return next;
}

core::Transform withKeys(core::Transform placed, TransformField field, std::vector<core::Keyframe> keys, double shown)
{
    core::KeyframedValue &value = fieldOf(placed, field);
    if (keys.empty())
        value.value = shown;
    value.keyframes = std::move(keys);
    return placed;
}

} // namespace ustudio::effects
