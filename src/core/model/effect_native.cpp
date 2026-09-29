#include "core/model/effect_native.h"

#include "core/model/animation.h"

#include <algorithm>
#include <cstdio>
#include <map>
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
    return !effect.mix.keyframes.empty() || effect.mix.value != 1.0 || effect.mask.has_value();
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

namespace {

// A mask as frei0r.alphaspot (the effects drop-in's frei0r; only when the
// caller says it's loaded): the shape drawn into the effect's alpha, the
// mix inside it and 0 outside (swapped when inverted), which mask_apply's
// cairoblend then composites over the frame from before the effect.
// Checked in a standalone repro (MLT 7.40, frei0r 2.5.6, 2026-09-28; docs/
// developer/notes/effects.md): parameter 0 is the shape (0 a rectangle,
// 0.3 an ellipse: frei0r scales 0-1 to four shapes), 1 and 2 the centre, 3
// and 4 the half-width and half-height, all fractions of the frame; 5 the
// tilt (0.5 upright), 6 the soft edge's width, 7 and 8 the alpha outside
// and inside.
NativeFilter maskFilter(const EffectMask &mask, const KeyframedValue &mixValue, FrameIndex offset, FrameIndex length)
{
    auto geometry = [&](const char *name, double fallback, double scale) {
        Param param{name, fallback, {}};
        for (const Param &p : mask.params)
            if (p.name == name)
                param = p;
        if (const double *v = std::get_if<double>(&param.value))
            param.value = *v * scale;
        for (Keyframe &key : param.keyframes)
            key.value *= scale;
        return nativeParam(param, offset, length);
    };
    Param mixParam{"mix", mixValue.value, mixValue.keyframes};
    Param feather{"feather", mask.feather.value, mask.feather.keyframes};
    const std::string inside = nativeParam(mixParam, offset, length);
    NativeFilter spot{"frei0r.alphaspot", {}};
    spot.properties = {
        {"0", mask.shape == "ellipse" ? "0.3" : "0"},
        {"1", geometry("x", 0.5, 1.0)},
        {"2", geometry("y", 0.5, 1.0)},
        {"3", geometry("width", 0.5, 0.5)},
        {"4", geometry("height", 0.5, 0.5)},
        {"5", "0.5"},
        {"6", nativeParam(feather, offset, length)},
        {"7", mask.invert ? inside : "0"},
        {"8", mask.invert ? "0" : inside},
    };
    return spot;
}

} // namespace

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
    const bool masked = effect.mask && mix == MixTransition::Cairoblend &&
                        (effect.mask->shape == "rectangle" || effect.mask->shape == "ellipse");
    if (masked) {
        filters.push_back(std::move(start));
        filters.push_back(maskFilter(*effect.mask, effect.mix, offset, length));
        // The shape's alpha carries the mix: the transition at full opacity.
        filters.push_back({"mask_apply", {{"transition", mixTransitionService(mix)}, {"transition.0", "1"}}});
        if (!effect.enabled)
            for (NativeFilter &filter : filters)
                filter.properties.emplace_back("disable", "1");
        return filters;
    }
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
    // Where the envelope is 1 (between the fades) the mix keeps its own keys
    // and easings. Inside the fades, and across any segment of the mix that
    // a fade boundary cuts, the product is sampled every frame (fades are
    // short), linear between samples.
    const FrameIndex last = block.length - 1;
    const FrameIndex flatStart = fadeIn ? std::min(block.fadeIn->length, last) : 0;
    const FrameIndex flatEnd = fadeOut ? std::max<FrameIndex>(last - block.fadeOut->length, 0) : last;
    for (Effect &effect : effects) {
        const std::vector<Keyframe> &own = effect.mix.keyframes;
        auto mixAt = [&](FrameIndex frame) {
            return own.empty() ? effect.mix.value : easedValue(own, static_cast<double>(frame));
        };
        std::set<FrameIndex> sampled;
        for (FrameIndex f = 0; fadeIn && f <= flatStart; ++f)
            sampled.insert(f);
        for (FrameIndex f = flatEnd; fadeOut && f <= last; ++f)
            sampled.insert(f);
        for (size_t i = 0; i + 1 < own.size(); ++i) {
            const FrameIndex a = own[i].at, b = own[i + 1].at;
            const bool cut = (fadeIn && a < flatStart && flatStart < b) || (fadeOut && a < flatEnd && flatEnd < b);
            for (FrameIndex f = std::max<FrameIndex>(a, 0); cut && f <= std::min(b, last); ++f)
                sampled.insert(f);
        }
        std::map<FrameIndex, Keyframe> keys;
        for (FrameIndex f : sampled)
            keys[f] = {f, mixAt(f) * envelope(block, static_cast<double>(f)), Easing::Linear};
        // The mix's own keys between the fades, with their easings: a key that
        // was also sampled keeps its easing when the segment after it isn't.
        for (size_t i = 0; i < own.size(); ++i) {
            const Keyframe &key = own[i];
            if (key.at < flatStart || key.at > flatEnd)
                continue;
            const bool nextSampled = i + 1 < own.size() && sampled.contains(std::min(key.at + 1, last)) &&
                                     sampled.contains(own[i + 1].at);
            if (!keys.contains(key.at) || !nextSampled)
                keys[key.at] = {key.at, key.value, nextSampled ? Easing::Linear : key.easing};
        }
        // Held values between the fades (before the first key, after the last).
        for (FrameIndex f : {flatStart, flatEnd})
            if (!keys.contains(f))
                keys[f] = {f, mixAt(f) * envelope(block, static_cast<double>(f)), Easing::Linear};
        effect.mix.keyframes.clear();
        for (auto &[frame, key] : keys)
            effect.mix.keyframes.push_back(key);
    }
    return effects;
}

} // namespace ustudio::core
