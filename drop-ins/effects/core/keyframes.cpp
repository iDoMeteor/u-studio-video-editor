#include "core/keyframes.h"

#include "core/model/animation.h"

#include <algorithm>

namespace ustudio::effects {

namespace {
auto keyPosition(std::vector<core::Keyframe> &keys, core::FrameIndex at)
{
    return std::lower_bound(keys.begin(), keys.end(), at,
                            [](const core::Keyframe &k, core::FrameIndex frame) { return k.at < frame; });
}
} // namespace

const core::Keyframe *keyAt(const std::vector<core::Keyframe> &keys, core::FrameIndex at)
{
    for (const core::Keyframe &k : keys)
        if (k.at == at)
            return &k;
    return nullptr;
}

std::vector<core::Keyframe> withKeyAt(std::vector<core::Keyframe> keys, core::FrameIndex at, double value,
                                      core::Easing easing)
{
    auto it = keyPosition(keys, at);
    if (it != keys.end() && it->at == at)
        it->value = value;
    else
        keys.insert(it, core::Keyframe{at, value, easing});
    return keys;
}

std::vector<core::Keyframe> withoutKeyAt(std::vector<core::Keyframe> keys, core::FrameIndex at)
{
    auto it = keyPosition(keys, at);
    if (it != keys.end() && it->at == at)
        keys.erase(it);
    return keys;
}

std::vector<core::Keyframe> withEasingAt(std::vector<core::Keyframe> keys, core::FrameIndex at, core::Easing easing)
{
    auto it = keyPosition(keys, at);
    if (it != keys.end() && it->at == at)
        it->easing = easing;
    return keys;
}

std::optional<core::FrameIndex> previousKey(const std::vector<core::Keyframe> &keys, core::FrameIndex at)
{
    std::optional<core::FrameIndex> found;
    for (const core::Keyframe &k : keys)
        if (k.at < at)
            found = k.at;
    return found;
}

std::optional<core::FrameIndex> nextKey(const std::vector<core::Keyframe> &keys, core::FrameIndex at)
{
    for (const core::Keyframe &k : keys)
        if (k.at > at)
            return k.at;
    return std::nullopt;
}

const std::vector<Feel> &feels()
{
    // Doc 15's chips, each one MLT easing.
    static const std::vector<Feel> list = {
        {"Linear", core::Easing::Linear},       {"Smooth", core::Easing::SmoothNatural},
        {"Ease in", core::Easing::CubicIn},     {"Ease out", core::Easing::CubicOut},
        {"Snap", core::Easing::ExponentialOut}, {"Bounce", core::Easing::BounceOut},
        {"Elastic", core::Easing::ElasticOut},  {"Hold", core::Easing::Discrete},
    };
    return list;
}

std::string feelName(core::Easing easing)
{
    for (const Feel &feel : feels())
        if (feel.easing == easing)
            return feel.name;
    return core::easingName(easing);
}

} // namespace ustudio::effects
