#include "evaluate.h"

#include "animation.h"

#include "core/model/animation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

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

double &stateSlot(LayerState &state, Property property)
{
    switch (property) {
    case Property::X:
        return state.x;
    case Property::Y:
        return state.y;
    case Property::Opacity:
        return state.opacity;
    case Property::Scale:
        return state.scale;
    case Property::Rotation:
        return state.rotation;
    case Property::Blur:
        return state.blur;
    case Property::Tracking:
        return state.tracking;
    case Property::Reveal:
        return state.reveal;
    case Property::Shift:
        return state.shift;
    case Property::FillR:
        return state.fill.r;
    case Property::FillG:
        return state.fill.g;
    case Property::FillB:
        return state.fill.b;
    case Property::FillA:
        return state.fill.a;
    case Property::ShadowOpacity:
        return state.shadowOpacity;
    }
    return state.opacity;
}

double stateValue(LayerState state, Property property)
{
    return stateSlot(state, property);
}

namespace {

// A track's keys at their places in the title, on core's keyframe model.
std::vector<core::Keyframe> absoluteKeys(const PropertyTrack &track, const Timing &timing)
{
    std::vector<core::Keyframe> keys;
    keys.reserve(track.keys.size());
    for (const TitleKey &key : track.keys) {
        core::Keyframe k = key.key;
        k.at = static_cast<core::FrameIndex>(keyPosition(timing, key));
        keys.push_back(k);
    }
    return keys;
}

} // namespace

LayerState evaluateLayer(const Layer &layer, const Timing &timing, double titleFrame)
{
    return evaluateLayer(layer, expandBehaviors(layer, timing), timing, titleFrame);
}

LayerState evaluateLayer(const Layer &layer, const Expansion &expansion, const Timing &timing, double titleFrame)
{
    LayerState state;
    state.x = layer.x;
    state.y = layer.y;
    state.opacity = layer.opacity;
    state.scale = layer.scale;
    state.rotation = layer.rotation;
    state.blur = layer.blur;
    state.tracking = layer.font.tracking;
    state.fill = layer.fill.color;
    // The layer's own keyframes: values.
    for (const PropertyTrack &track : layer.animation)
        if (!track.keys.empty())
            stateSlot(state, track.property) = core::easedValue(absoluteKeys(track, timing), titleFrame);
    // Its behaviours: offsets.
    for (const PropertyTrack &track : expansion.offsets) {
        if (track.keys.empty())
            continue;
        const double offset = core::easedValue(absoluteKeys(track, timing), titleFrame);
        double &value = stateSlot(state, track.property);
        value = multiplicative(track.property) ? value * offset : value + offset;
    }
    // Its loops, through the hold, ramped in and out at its edges.
    const double weight = expansion.loops.empty() ? 0.0 : loopWeight(timing, titleFrame);
    if (weight > 0.0) {
        for (const Loop &loop : expansion.loops) {
            const double wave =
                loop.centre + loop.amplitude * loopWave(loop, titleFrame - static_cast<double>(timing.intro));
            double &value = stateSlot(state, loop.property);
            if (multiplicative(loop.property))
                value *= 1.0 + (wave - 1.0) * weight;
            else
                value += wave * weight;
        }
    }
    state.opacity = std::clamp(state.opacity, 0.0, 1.0);
    state.reveal = std::clamp(state.reveal, 0.0, 1.0);
    state.shadowOpacity = std::max(0.0, state.shadowOpacity);
    state.blur = std::max(0.0, state.blur);
    for (double *channel : {&state.fill.r, &state.fill.g, &state.fill.b, &state.fill.a})
        *channel = std::clamp(*channel, 0.0, 1.0);
    return state;
}

namespace {

// Two digits, or more when the value needs them (a 100-minute countdown).
std::string twoDigits(long long value)
{
    char buffer[24];
    std::snprintf(buffer, sizeof buffer, "%02lld", value);
    return buffer;
}

// Whole seconds as MM:SS, or H:MM:SS from an hour.
std::string clockTime(long long seconds)
{
    if (seconds >= 3600)
        return std::to_string(seconds / 3600) + ":" + twoDigits(seconds / 60 % 60) + ":" + twoDigits(seconds % 60);
    return twoDigits(seconds / 60) + ":" + twoDigits(seconds % 60);
}

long long wholeSeconds(double frames, double fps)
{
    return fps > 0.0 ? static_cast<long long>(std::floor(std::max(0.0, frames) / fps)) : 0;
}

// Non-drop-frame at the nominal rate (30 for 29.97), as MLT's own
// timecode strings are.
std::string timecode(double frame, double fps)
{
    const long long rate = std::max(1LL, std::llround(fps));
    const auto frames = static_cast<long long>(std::floor(std::max(0.0, frame)));
    const long long seconds = frames / rate;
    return twoDigits(seconds / 3600) + ":" + twoDigits(seconds / 60 % 60) + ":" + twoDigits(seconds % 60) + ":" +
           twoDigits(frames % rate);
}

// "mm:ss" (or "ss", "hh:mm:ss") as seconds and its number of parts; none
// when it isn't one.
std::optional<std::pair<long long, int>> countdownSpan(const std::string &spec)
{
    long long total = 0;
    int parts = 0;
    size_t pos = 0;
    while (true) {
        const size_t colon = spec.find(':', pos);
        const std::string part = spec.substr(pos, colon == std::string::npos ? std::string::npos : colon - pos);
        if (part.empty() || part.size() > 6 ||
            !std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return std::nullopt;
        total = total * 60 + std::stoll(part);
        if (++parts > 3)
            return std::nullopt;
        if (colon == std::string::npos)
            break;
        pos = colon + 1;
    }
    return std::pair{total, parts};
}

std::string countdown(long long remaining, int parts)
{
    if (parts == 1)
        return twoDigits(remaining);
    if (parts == 2)
        return twoDigits(remaining / 60) + ":" + twoDigits(remaining % 60);
    return twoDigits(remaining / 3600) + ":" + twoDigits(remaining / 60 % 60) + ":" + twoDigits(remaining % 60);
}

// A dynamic field's text, or none when `name` isn't one.
std::optional<std::string> dynamicField(const std::string &name, const FieldClock &clock)
{
    if (name == "timecode")
        return timecode(clock.timelineFrame, clock.fps);
    if (name == "clip_time")
        return clockTime(wholeSeconds(clock.clipFrame, clock.fps));
    if (name.starts_with("countdown:")) {
        const auto span = countdownSpan(name.substr(10));
        if (!span)
            return std::nullopt;
        const long long remaining = std::max(0LL, span->first - wholeSeconds(clock.clipFrame, clock.fps));
        return countdown(remaining, span->second);
    }
    if (name == "date" || name.starts_with("date:")) {
        const std::string format = name.size() > 5 ? name.substr(5) : "%Y-%m-%d";
        char buffer[256];
        const size_t written = std::strftime(buffer, sizeof buffer, format.c_str(), &clock.localTime);
        return std::string(buffer, written);
    }
    return std::nullopt;
}

} // namespace

std::string substituteFields(const std::string &text, const std::vector<Field> &fields,
                             const std::map<std::string, std::string> &values, const FieldClock &clock)
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
        if (auto dynamic = dynamicField(name, clock)) {
            out += *dynamic;
        } else if (auto value = values.find(name); value != values.end()) {
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

bool hasDynamicFields(const std::string &text)
{
    size_t pos = 0;
    while ((pos = text.find("{{", pos)) != std::string::npos) {
        const size_t close = text.find("}}", pos + 2);
        if (close == std::string::npos)
            return false;
        if (dynamicField(text.substr(pos + 2, close - pos - 2), FieldClock{}))
            return true;
        pos = close + 2;
    }
    return false;
}

} // namespace ustudio::titles
