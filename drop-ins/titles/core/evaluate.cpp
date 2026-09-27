#include "evaluate.h"

#include "core/model/animation.h"

#include <algorithm>

namespace ustudio::titles {

double titleFrame(const TitleDocument &doc, double clipLength, double clipFrame, double clipFps)
{
    const double docFps = doc.fpsDen > 0 ? static_cast<double>(doc.fpsNum) / doc.fpsDen : 0.0;
    if (clipLength <= 0.0 || clipFps <= 0.0 || docFps <= 0.0)
        return 0.0;
    // Everything in title frames from here.
    const double rate = docFps / clipFps;
    const double length = clipLength * rate;
    const double at = std::clamp(clipFrame, 0.0, clipLength) * rate;
    const auto intro = static_cast<double>(doc.timing.intro);
    const auto hold = static_cast<double>(doc.timing.hold);
    const auto outro = static_cast<double>(doc.timing.outro);

    if (length < intro + outro) {
        const double squeeze = length / (intro + outro);
        if (at < intro * squeeze)
            return at / squeeze;
        return intro + hold + (at - intro * squeeze) / squeeze;
    }
    if (at < intro)
        return at;
    const double outroStart = length - outro;
    if (at >= outroStart)
        return intro + hold + (at - outroStart);
    const double clipHold = outroStart - intro;
    return intro + (at - intro) * hold / clipHold;
}

double keyPosition(const Timing &timing, const TitleKey &key)
{
    const auto at = static_cast<double>(key.key.at);
    switch (key.zone) {
    case Zone::Intro:
        return at;
    case Zone::Hold:
        return static_cast<double>(timing.intro) + at;
    case Zone::Outro:
        return static_cast<double>(timing.intro + timing.hold) + at;
    }
    return at;
}

LayerState evaluateLayer(const Layer &layer, const Timing &timing, double titleFrame)
{
    LayerState state{layer.x, layer.y, layer.opacity, layer.scale, layer.rotation};
    for (const PropertyTrack &track : layer.animation) {
        if (track.keys.empty())
            continue;
        // core::Keyframe positions are whole frames; zones are too, so the
        // absolute positions are exact.
        std::vector<core::Keyframe> keys;
        keys.reserve(track.keys.size());
        for (const TitleKey &key : track.keys) {
            core::Keyframe k = key.key;
            k.at = static_cast<core::FrameIndex>(keyPosition(timing, key));
            keys.push_back(k);
        }
        const double value = core::easedValue(keys, titleFrame);
        switch (track.property) {
        case Property::X:
            state.x = value;
            break;
        case Property::Y:
            state.y = value;
            break;
        case Property::Opacity:
            state.opacity = value;
            break;
        case Property::Scale:
            state.scale = value;
            break;
        case Property::Rotation:
            state.rotation = value;
            break;
        }
    }
    state.opacity = std::clamp(state.opacity, 0.0, 1.0);
    return state;
}

std::string substituteFields(const std::string &text, const std::vector<Field> &fields,
                             const std::map<std::string, std::string> &values)
{
    std::string out;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t open = text.find("{{", pos);
        if (open == std::string::npos)
            break;
        const size_t close = text.find("}}", open + 2);
        if (close == std::string::npos)
            break;
        out.append(text, pos, open - pos);
        const std::string name = text.substr(open + 2, close - open - 2);
        if (auto value = values.find(name); value != values.end()) {
            out += value->second;
        } else if (auto field =
                       std::find_if(fields.begin(), fields.end(), [&](const Field &f) { return f.name == name; });
                   field != fields.end()) {
            out += field->defaultValue;
        } else {
            out.append(text, open, close + 2 - open);
        }
        pos = close + 2;
    }
    out.append(text, pos, std::string::npos);
    return out;
}

} // namespace ustudio::titles
