#include "core/model/transform.h"

#include "core/model/animation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ustudio::core {

namespace {

// Transform's animatable values, in declaration order.
constexpr KeyframedValue Transform::*kValues[] = {&Transform::x,       &Transform::y,         &Transform::width,
                                                  &Transform::height,  &Transform::rotation,  &Transform::cropLeft,
                                                  &Transform::cropTop, &Transform::cropRight, &Transform::cropBottom};

double frameAspect(const Profile &profile)
{
    return profile.dar.num > 0 && profile.dar.den > 0 ? static_cast<double>(profile.dar.num) / profile.dar.den
                                                      : static_cast<double>(profile.width) / profile.height;
}

} // namespace

double croppedWidth(const Transform &t, int sourceWidth)
{
    return std::max(1.0, sourceWidth - t.cropLeft.value - t.cropRight.value);
}

double croppedHeight(const Transform &t, int sourceHeight)
{
    return std::max(1.0, sourceHeight - t.cropTop.value - t.cropBottom.value);
}

Placement placementFor(const Transform &t, int sourceWidth, int sourceHeight, const Profile &profile)
{
    const int srcW = sourceWidth > 0 ? sourceWidth : profile.width;
    const int srcH = sourceHeight > 0 ? sourceHeight : profile.height;
    Placement p;
    p.rotation = t.rotation.value;
    switch (t.bounds) {
    case Transform::Bounds::Stretch:
        p = {profile.width / 2.0, profile.height / 2.0, static_cast<double>(profile.width),
             static_cast<double>(profile.height), t.rotation.value};
        break;
    case Transform::Bounds::Fit: {
        // Square pixels on screen: the frame's width in "display" units is
        // height * aspect, which is profile.width for square-pixel profiles.
        const double cw = croppedWidth(t, srcW), ch = croppedHeight(t, srcH);
        const double frameW = profile.height * frameAspect(profile), frameH = profile.height;
        const double scale = std::min(frameW / cw, frameH / ch);
        const double sx = profile.width / frameW; // back to profile pixels
        p = {profile.width / 2.0, profile.height / 2.0, cw * scale * sx, ch * scale, t.rotation.value};
        break;
    }
    case Transform::Bounds::None:
        p = {t.x.value, t.y.value, t.width.value, t.height.value, t.rotation.value};
        break;
    }
    return p;
}

Transform transformAt(const Transform &t, FrameIndex frame)
{
    Transform out = t;
    for (KeyframedValue Transform::*member : kValues) {
        KeyframedValue &v = out.*member;
        if (v.keyframes.empty())
            continue;
        v.value = easedValue(v.keyframes, static_cast<double>(frame));
        v.keyframes.clear();
    }
    return out;
}

Placement placementAt(const Transform &t, FrameIndex frame, int sourceWidth, int sourceHeight, const Profile &profile)
{
    return placementFor(transformAt(t, frame), sourceWidth, sourceHeight, profile);
}

Transform withTransformAt(const Transform &t, FrameIndex frame, const Transform &placed)
{
    Transform out = t;
    out.bounds = placed.bounds;
    out.flipH = placed.flipH;
    out.flipV = placed.flipV;
    for (KeyframedValue Transform::*member : kValues) {
        KeyframedValue &v = out.*member;
        const double value = (placed.*member).value;
        if (v.keyframes.empty()) {
            v.value = value;
            continue;
        }
        auto at = std::lower_bound(v.keyframes.begin(), v.keyframes.end(), frame,
                                   [](const Keyframe &k, FrameIndex f) { return k.at < f; });
        if (at != v.keyframes.end() && at->at == frame)
            at->value = value;
        else
            v.keyframes.insert(at, Keyframe{frame, value, Easing::Linear});
    }
    return out;
}

Transform explicitTransform(const Transform &t, int sourceWidth, int sourceHeight, const Profile &profile)
{
    if (t.bounds == Transform::Bounds::None)
        return t;
    const Placement p = placementFor(t, sourceWidth, sourceHeight, profile);
    Transform out = t;
    out.bounds = Transform::Bounds::None;
    out.x.value = p.cx;
    out.y.value = p.cy;
    out.width.value = p.w;
    out.height.value = p.h;
    return out;
}

bool isAnimated(const Transform &t)
{
    for (KeyframedValue Transform::*member : kValues)
        if (!(t.*member).keyframes.empty())
            return true;
    return false;
}

bool isIdentity(const Transform &t, int sourceWidth, int sourceHeight, const Profile &profile)
{
    if (isAnimated(t))
        return false;
    // The track compositor (composite, fill=1) scales a picture of the
    // frame's aspect to fill it, so Fit or Stretch of one is no filter. It
    // keeps another aspect's shape, so Stretch of one needs the affine
    // filter (Fit may not: compositorFits()). Unknown sizes (not probed yet)
    // count as matching.
    const bool sameAspect = sourceWidth <= 0 || sourceHeight <= 0 || profile.width <= 0 || profile.height <= 0 ||
                            std::abs(static_cast<long long>(sourceWidth) * profile.height -
                                     static_cast<long long>(sourceHeight) * profile.width) <=
                                static_cast<long long>(std::max(profile.width, profile.height));
    return t.bounds != Transform::Bounds::None && sameAspect && !t.flipH && !t.flipV && t.rotation.value == 0.0 &&
           t.cropLeft.value == 0.0 && t.cropTop.value == 0.0 && t.cropRight.value == 0.0 && t.cropBottom.value == 0.0;
}

bool compositorFits(const Transform &t)
{
    return !isAnimated(t) && t.bounds == Transform::Bounds::Fit && !t.flipH && !t.flipV && t.rotation.value == 0.0 &&
           t.cropLeft.value == 0.0 && t.cropTop.value == 0.0 && t.cropRight.value == 0.0 && t.cropBottom.value == 0.0;
}

bool hasTransformedClip(const Project &project)
{
    for (const Sequence &sequence : project.sequences)
        if (sequence.id == project.activeSequence)
            for (const auto &[id, clip] : sequence.clips)
                if (!(clip.transform.get() == Transform{}))
                    return true;
    return false;
}

std::string transformProblem(const Transform &t)
{
    for (const KeyframedValue *v :
         {&t.x, &t.y, &t.width, &t.height, &t.rotation, &t.cropLeft, &t.cropTop, &t.cropRight, &t.cropBottom})
        if (!std::isfinite(v->value))
            return "a transform value isn't a number";
    if (t.bounds == Transform::Bounds::None && (t.width.value <= 0 || t.height.value <= 0))
        return "an explicitly placed picture needs a positive size";
    for (const KeyframedValue *crop : {&t.cropLeft, &t.cropTop, &t.cropRight, &t.cropBottom})
        if (crop->value < 0)
            return "a crop can't be negative";
    if (!isAnimated(t))
        return {};
    // Keyframes: on a placed picture only (Fit and Stretch compute the
    // placement), never on a crop (the crop filter doesn't animate).
    for (const KeyframedValue *crop : {&t.cropLeft, &t.cropTop, &t.cropRight, &t.cropBottom})
        if (!crop->keyframes.empty())
            return "a crop can't be keyframed";
    if (t.bounds != Transform::Bounds::None)
        return "only an explicitly placed picture can be keyframed";
    for (KeyframedValue Transform::*member : kValues) {
        const std::vector<Keyframe> &keys = (t.*member).keyframes;
        for (size_t i = 0; i < keys.size(); ++i) {
            if (!std::isfinite(keys[i].value))
                return "a transform keyframe isn't a number";
            if (i > 0 && keys[i].at <= keys[i - 1].at)
                return "transform keyframes must be in order, one per frame";
        }
    }
    for (const KeyframedValue *size : {&t.width, &t.height})
        for (const Keyframe &k : size->keyframes)
            if (k.value <= 0)
                return "an explicitly placed picture needs a positive size";
    return {};
}

namespace {

std::string number(double value)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4f", value);
    std::string text = buf;
    while (text.back() == '0')
        text.pop_back();
    if (text.back() == '.')
        text.pop_back();
    return text == "-0" ? "0" : text;
}

// affine's "x y w h 1" for a centre-placed picture, in output pixels.
std::string rectText(double cx, double cy, double w, double h, double outputScale)
{
    return number((cx - w / 2) * outputScale) + " " + number((cy - h / 2) * outputScale) + " " +
           number(w * outputScale) + " " + number(h * outputScale) + " 1";
}

// The animated rect of a placed (None) transform on a cut `offset` frames
// into its clip, `length` long (transformFilters()).
std::string animatedRect(const Transform &t, FrameIndex offset, FrameIndex length, double outputScale)
{
    const KeyframedValue *values[] = {&t.x, &t.y, &t.width, &t.height};
    const KeyframedValue *keyed = nullptr;
    bool aligned = true;
    FrameIndex first = 0, last = 0;
    for (const KeyframedValue *v : values) {
        if (v->keyframes.empty())
            continue;
        if (!keyed) {
            keyed = v;
            first = v->keyframes.front().at;
            last = v->keyframes.back().at;
            continue;
        }
        first = std::min(first, v->keyframes.front().at);
        last = std::max(last, v->keyframes.back().at);
        aligned = aligned && v->keyframes.size() == keyed->keyframes.size() &&
                  std::equal(v->keyframes.begin(), v->keyframes.end(), keyed->keyframes.begin(),
                             [](const Keyframe &a, const Keyframe &b) { return a.at == b.at && a.easing == b.easing; });
    }
    auto at = [&](FrameIndex frame, int component) {
        const KeyframedValue &v = *values[component];
        return v.keyframes.empty() ? v.value : easedValue(v.keyframes, static_cast<double>(frame));
    };
    if (!keyed)
        return rectText(t.x.value, t.y.value, t.width.value, t.height.value, outputScale);
    std::string out;
    auto add = [&](FrameIndex position, Easing easing, double cx, double cy, double w, double h) {
        out += (out.empty() ? "" : ";") + std::to_string(position) + easingOperator(easing) + "=" +
               rectText(cx, cy, w, h, outputScale);
    };
    if (aligned) {
        // The same keys for every keyed value: each rect key eases all four
        // numbers with one easing, as each value would alone (the rect is
        // linear in them).
        std::vector<std::vector<Keyframe>> cut(4);
        for (int c = 0; c < 4; ++c)
            if (!values[c]->keyframes.empty())
                cut[static_cast<size_t>(c)] = keyframesForCut(values[c]->keyframes, offset, length);
        const std::vector<Keyframe> &frames = keyframesForCut(keyed->keyframes, offset, length);
        for (size_t i = 0; i < frames.size(); ++i) {
            double v[4];
            for (int c = 0; c < 4; ++c)
                v[c] = cut[static_cast<size_t>(c)].empty() ? values[c]->value : cut[static_cast<size_t>(c)][i].value;
            add(frames[i].at, frames[i].easing, v[0], v[1], v[2], v[3]);
        }
        return out;
    }
    // Keys on different frames or easings: every frame between the first
    // and last key, linear between whole frames, which is exact where MLT
    // samples; outside them the values hold.
    const FrameIndex from = std::clamp(first - offset, FrameIndex{0}, length - 1);
    const FrameIndex to = std::clamp(last - offset, FrameIndex{0}, length - 1);
    for (FrameIndex f = from; f <= to; ++f)
        add(f, Easing::Linear, at(f + offset, 0), at(f + offset, 1), at(f + offset, 2), at(f + offset, 3));
    return out;
}

} // namespace

std::vector<NativeFilter> transformFilters(const Transform &input, int sourceWidth, int sourceHeight,
                                           const Profile &profile, double outputScale, double sourceScale,
                                           FrameIndex offset, FrameIndex length)
{
    std::vector<NativeFilter> filters;
    // Only a placed picture animates (check() refuses keys elsewhere, and
    // on crops); anything else is taken at the cut's start.
    const bool animated = isAnimated(input) && input.bounds == Transform::Bounds::None && length > 0;
    const Transform t = animated ? input : transformAt(input, offset);
    if (isIdentity(t, sourceWidth, sourceHeight, profile))
        return filters;
    auto pixels = [&](double sourcePixels) { return std::to_string(std::lround(sourcePixels * sourceScale)); };
    if (t.cropLeft.value > 0 || t.cropTop.value > 0 || t.cropRight.value > 0 || t.cropBottom.value > 0)
        filters.push_back({"crop",
                           {{"left", pixels(t.cropLeft.value)},
                            {"top", pixels(t.cropTop.value)},
                            {"right", pixels(t.cropRight.value)},
                            {"bottom", pixels(t.cropBottom.value)}}});
    if (t.flipH)
        filters.push_back({"mirror", {{"mirror", "flip"}}});
    if (t.flipV)
        filters.push_back({"mirror", {{"mirror", "flop"}}});
    const Placement p = placementFor(t, sourceWidth, sourceHeight, profile);
    // use_normalized: the picture is drawn on a frame-sized canvas, whatever
    // size the compositor asks for, with the (cropped) source stretched to
    // it, so `rect` is exactly where the picture goes. Without it the
    // canvas was the source's own size and every placement came out scaled
    // by source/frame. repeat_off/mirror_off: affine otherwise tiles and
    // mirrors the picture outside `rect` (as kdenlive's pan_zoom sets them).
    // All verified with standalone repros (MLT 7.40, 2026-09-25).
    filters.push_back(
        {"affine",
         {{"use_normalized", "1"},
          {"transition.rect",
           animated ? animatedRect(t, offset, length, outputScale) : rectText(p.cx, p.cy, p.w, p.h, outputScale)},
          {"transition.distort", "1"},
          {"transition.repeat_off", "1"},
          {"transition.mirror_off", "1"},
          {"transition.fix_rotate_x", animated && !t.rotation.keyframes.empty()
                                          ? animationString(keyframesForCut(t.rotation.keyframes, offset, length))
                                          : number(p.rotation)}}});
    return filters;
}

std::vector<NativeFilter> gpuTransformFilters(const Transform &t, int sourceWidth, int sourceHeight,
                                              const Profile &profile, double outputScale, double sourceScale,
                                              FrameIndex offset, FrameIndex length)
{
    std::vector<NativeFilter> filters =
        transformFilters(t, sourceWidth, sourceHeight, profile, outputScale, sourceScale, offset, length);
    for (const NativeFilter &filter : filters)
        for (const auto &[name, value] : filter.properties)
            if (name == "transition.fix_rotate_x" && value != "0")
                return filters;
    for (NativeFilter &filter : filters) {
        if (filter.service == "mirror") {
            const bool horizontal = filter.properties.front().second == "flip";
            filter = {horizontal ? "movit.mirror" : "movit.flip", {}};
        } else if (filter.service == "affine") {
            // "x y w h 1": the same rect without affine's opacity, from
            // every key of an animated one. Property names from
            // /usr/share/mlt-7/movit/filter_movit_rect.yml.
            std::string rect;
            for (const auto &[name, value] : filter.properties)
                if (name == "transition.rect")
                    for (size_t start = 0; start < value.size();) {
                        size_t end = value.find(';', start);
                        if (end == std::string::npos)
                            end = value.size();
                        const std::string key = value.substr(start, end - start);
                        rect += (rect.empty() ? "" : ";") + key.substr(0, key.rfind(' '));
                        start = end + 1;
                    }
            filter = {"movit.rect", {{"rect", rect}, {"distort", "1"}}};
        }
    }
    return filters;
}

} // namespace ustudio::core
