#include "core/model/effect_native.h"

#include "core/model/animation.h"

#include <algorithm>
#include <cstdio>
#include <set>

namespace ustudio::core {

namespace {

// affine's rect with the mix as its opacity: the whole frame, "o%" last.
std::string affineRect(double mix)
{
    return "0% 0% 100% 100% " + formatDouble(mix * 100.0) + "%";
}

} // namespace

const char *mixTransitionService(MixTransition transition)
{
    return transition == MixTransition::Cairoblend ? "frei0r.cairoblend" : "affine";
}

bool needsMixWrap(const Effect &effect)
{
    return !effect.mix.keyframes.empty() || effect.mix.value != 1.0;
}

std::string nativeValue(const Param::Value &value)
{
    return std::visit(
        [](const auto &v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, double>)
                return formatDouble(v);
            else if constexpr (std::is_same_v<T, int64_t>)
                return std::to_string(v);
            else if constexpr (std::is_same_v<T, bool>)
                return v ? "1" : "0";
            else if constexpr (std::is_same_v<T, std::string>)
                return v;
            else if constexpr (std::is_same_v<T, Color>) {
                char buf[16]; // MLT's colour form: 0xRRGGBBAA
                std::snprintf(buf, sizeof buf, "0x%02x%02x%02x%02x", v.r, v.g, v.b, v.a);
                return buf;
            } else
                return formatDouble(v.x) + " " + formatDouble(v.y) + " " + formatDouble(v.w) + " " + formatDouble(v.h);
        },
        value);
}

std::string nativeParam(const Param &param, FrameIndex offset, FrameIndex length)
{
    if (!param.keyframes.empty() && std::holds_alternative<double>(param.value))
        return animationString(keyframesForCut(param.keyframes, offset, length));
    return nativeValue(param.value);
}

std::vector<NativeFilter> nativeFilters(const Effect &effect, FrameIndex offset, FrameIndex length, MixTransition mix)
{
    std::vector<NativeFilter> filters;
    if (!needsMixWrap(effect)) {
        NativeFilter filter{effect.service, {}};
        for (const Param &param : effect.params)
            if (!param.name.empty())
                filter.properties.emplace_back(param.name, nativeParam(param, offset, length));
        if (!effect.enabled)
            filter.properties.emplace_back("disable", "1");
        filters.push_back(std::move(filter));
        return filters;
    }
    NativeFilter start{"mask_start", {{"filter", effect.service}}};
    for (const Param &param : effect.params)
        if (!param.name.empty())
            start.properties.emplace_back("filter." + param.name, nativeParam(param, offset, length));
    // A constant mix is the transition's opacity. A keyframed one can't be:
    // mask_apply runs its transition when the image is fetched, by which
    // time a playlist has given the frame its timeline position, so an
    // animated transition property on a cut reads the wrong frame (constant
    // in practice). A filter's animation position is fixed when the frame is
    // processed, so the keyframes go on brightness's "alpha" between the
    // pair, and the transition composites the result at full opacity by its
    // alpha (standalone repro, MLT 7.40, 2026-09-28; docs/developer/notes/
    // effects.md).
    const bool animated = !effect.mix.keyframes.empty();
    const double opacity = animated ? 1.0 : effect.mix.value;
    NativeFilter apply{"mask_apply", {{"transition", mixTransitionService(mix)}}};
    if (mix == MixTransition::Cairoblend)
        apply.properties.emplace_back("transition.0", formatDouble(opacity));
    else
        apply.properties.emplace_back("transition.rect", affineRect(opacity));
    filters.push_back(std::move(start));
    if (animated)
        filters.push_back(
            {"brightness",
             {{"level", "1"}, {"alpha", animationString(keyframesForCut(effect.mix.keyframes, offset, length))}}});
    filters.push_back(std::move(apply));
    if (!effect.enabled)
        for (NativeFilter &filter : filters)
            filter.properties.emplace_back("disable", "1");
    return filters;
}

namespace {

// The fade envelope at `frame` of a block `length` long.
double envelope(const AdjustmentBlock &block, double frame)
{
    double gain = 1.0;
    const double last = static_cast<double>(block.length - 1);
    if (block.fadeIn && block.fadeIn->length > 0)
        gain = std::min(gain, frame / static_cast<double>(block.fadeIn->length));
    if (block.fadeOut && block.fadeOut->length > 0)
        gain = std::min(gain, (last - frame) / static_cast<double>(block.fadeOut->length));
    return std::clamp(gain, 0.0, 1.0);
}

} // namespace

std::vector<Effect> blockEffects(const AdjustmentBlock &block)
{
    std::vector<Effect> effects = block.effects;
    const bool fadeIn = block.fadeIn && block.fadeIn->length > 0;
    const bool fadeOut = block.fadeOut && block.fadeOut->length > 0;
    if (!fadeIn && !fadeOut)
        return effects;
    // Keys where the envelope bends and wherever the mix had its own, each
    // the mix there times the envelope (linear between: the envelope is
    // linear, and the mix's own keys are kept as points).
    const FrameIndex last = block.length - 1;
    for (Effect &effect : effects) {
        std::set<FrameIndex> at{0, last};
        if (fadeIn)
            at.insert(std::min(block.fadeIn->length, last));
        if (fadeOut)
            at.insert(std::max<FrameIndex>(last - block.fadeOut->length, 0));
        for (const Keyframe &key : effect.mix.keyframes)
            if (key.at >= 0 && key.at <= last)
                at.insert(key.at);
        std::vector<Keyframe> keys;
        for (FrameIndex frame : at) {
            const double mix = effect.mix.keyframes.empty()
                                   ? effect.mix.value
                                   : easedValue(effect.mix.keyframes, static_cast<double>(frame));
            keys.push_back({frame, mix * envelope(block, static_cast<double>(frame)), Easing::Linear});
        }
        effect.mix.keyframes = std::move(keys);
    }
    return effects;
}

} // namespace ustudio::core
