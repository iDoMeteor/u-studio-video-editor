#include "core/keyframes.h"

#include "core/model/animation.h"

#include <algorithm>
#include <cmath>

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

namespace {

// Keeps the points of performed[from..to] the line from `from` to `to`
// can't stand in for (within `tolerance`), recursively.
void thin(const std::vector<std::pair<core::FrameIndex, double>> &points, size_t from, size_t to, double tolerance,
          std::vector<bool> &keep)
{
    if (to <= from + 1)
        return;
    const auto [x0, y0] = points[from];
    const auto [x1, y1] = points[to];
    double worst = -1.0;
    size_t worstAt = from;
    for (size_t i = from + 1; i < to; ++i) {
        const double t = x1 == x0 ? 0.0 : static_cast<double>(points[i].first - x0) / static_cast<double>(x1 - x0);
        const double error = std::abs(points[i].second - (y0 + (y1 - y0) * t));
        if (error > worst) {
            worst = error;
            worstAt = i;
        }
    }
    if (worst <= tolerance)
        return;
    keep[worstAt] = true;
    thin(points, from, worstAt, tolerance, keep);
    thin(points, worstAt, to, tolerance, keep);
}

} // namespace

std::vector<core::Keyframe> withRecording(std::vector<core::Keyframe> keys,
                                          const std::vector<std::pair<core::FrameIndex, double>> &performed,
                                          double tolerance)
{
    if (performed.empty())
        return keys;
    // In frame order, one value per frame (the last performed wins).
    std::vector<std::pair<core::FrameIndex, double>> points = performed;
    std::stable_sort(points.begin(), points.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    std::vector<std::pair<core::FrameIndex, double>> unique;
    for (const auto &point : points) {
        if (!unique.empty() && unique.back().first == point.first)
            unique.back() = point;
        else
            unique.push_back(point);
    }
    const core::FrameIndex first = unique.front().first, last = unique.back().first;
    std::erase_if(keys, [&](const core::Keyframe &key) { return key.at >= first && key.at <= last; });
    std::vector<bool> keep(unique.size(), false);
    keep.front() = keep.back() = true;
    thin(unique, 0, unique.size() - 1, tolerance, keep);
    for (size_t i = 0; i < unique.size(); ++i)
        if (keep[i])
            keys.push_back(core::Keyframe{unique[i].first, unique[i].second, core::Easing::Linear});
    std::sort(keys.begin(), keys.end(), [](const core::Keyframe &a, const core::Keyframe &b) { return a.at < b.at; });
    return keys;
}

double recordingTolerance(double minimum, double maximum)
{
    return std::max(std::abs(maximum - minimum) * 0.005, 1e-6);
}

} // namespace ustudio::effects
