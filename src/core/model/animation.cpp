#include "core/model/animation.h"

#include <array>
#include <charconv>

namespace ustudio::core {

namespace {
// Indexed by Easing's value, which is mlt_keyframe_type's.
constexpr std::array<const char *, 35> kOperators = {
    "|", "",  "~", "$", "-", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
    "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "A", "B", "C", "D",
};
static_assert(static_cast<size_t>(Easing::BounceInOut) + 1 == kOperators.size());

double interpolate(const Keyframe &before, const Keyframe &after, FrameIndex at)
{
    if (after.at == before.at)
        return after.value;
    const double t = static_cast<double>(at - before.at) / static_cast<double>(after.at - before.at);
    return before.easing == Easing::Discrete ? before.value : before.value + (after.value - before.value) * t;
}
} // namespace

const char *easingOperator(Easing easing)
{
    const size_t index = static_cast<size_t>(easing);
    return index < kOperators.size() ? kOperators[index] : "";
}

Easing easingFromOperator(char op)
{
    for (size_t i = 0; i < kOperators.size(); ++i)
        if (kOperators[i][0] != '\0' && kOperators[i][0] == op)
            return static_cast<Easing>(i);
    if (op == '!')
        return Easing::Discrete; // MLT's other spelling of discrete
    return Easing::Linear;
}

std::vector<Keyframe> keyframesForCut(const std::vector<Keyframe> &keyframes, FrameIndex offset, FrameIndex length)
{
    std::vector<Keyframe> cut;
    if (keyframes.empty() || length <= 0)
        return cut;
    const FrameIndex last = length - 1;
    for (size_t i = 0; i < keyframes.size(); ++i) {
        Keyframe k = keyframes[i];
        k.at -= offset;
        if (k.at < 0) {
            // The last keyframe before the cut sets the value at its start.
            const bool nextInside = i + 1 < keyframes.size() && keyframes[i + 1].at - offset > 0;
            if (i + 1 == keyframes.size() || nextInside) {
                Keyframe edge = k;
                edge.at = 0;
                if (i + 1 < keyframes.size()) {
                    Keyframe next = keyframes[i + 1];
                    next.at -= offset;
                    edge.value = interpolate(k, next, 0);
                }
                cut.push_back(edge);
            }
            continue;
        }
        if (k.at > last) {
            // The first keyframe past the cut sets the value at its end.
            if (!cut.empty() && cut.back().at < last) {
                Keyframe edge = cut.back();
                edge.value = interpolate(cut.back(), k, last);
                edge.at = last;
                cut.push_back(edge);
            } else if (cut.empty()) {
                Keyframe edge = k;
                edge.at = 0;
                cut.push_back(edge);
            }
            break;
        }
        cut.push_back(k);
    }
    return cut;
}

std::string formatDouble(double value)
{
    std::array<char, 64> buf{};
    auto result = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    return std::string(buf.data(), result.ptr);
}

std::string animationString(const std::vector<Keyframe> &keyframes)
{
    std::string out;
    for (const Keyframe &k : keyframes) {
        if (!out.empty())
            out += ';';
        out += std::to_string(k.at) + easingOperator(k.easing) + "=" + formatDouble(k.value);
    }
    return out;
}

} // namespace ustudio::core
