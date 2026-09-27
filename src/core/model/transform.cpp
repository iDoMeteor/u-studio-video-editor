#include "core/model/transform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ustudio::core {

namespace {

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

bool isIdentity(const Transform &t, int sourceWidth, int sourceHeight, const Profile &profile)
{
    // The track compositor (composite, fill=1) scales a picture of the
    // frame's aspect to fill it, so Fit or Stretch of one is no filter; any
    // other aspect is fitted by the affine filter (composite would leave it
    // at the left). Unknown sizes (not probed yet) count as matching.
    const bool sameAspect = sourceWidth <= 0 || sourceHeight <= 0 || profile.width <= 0 || profile.height <= 0 ||
                            std::abs(static_cast<long long>(sourceWidth) * profile.height -
                                     static_cast<long long>(sourceHeight) * profile.width) <=
                                static_cast<long long>(std::max(profile.width, profile.height));
    return t.bounds != Transform::Bounds::None && sameAspect && !t.flipH && !t.flipV && t.rotation.value == 0.0 &&
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
    return {};
}

std::vector<NativeFilter> transformFilters(const Transform &t, int sourceWidth, int sourceHeight,
                                           const Profile &profile, double outputScale, double sourceScale)
{
    std::vector<NativeFilter> filters;
    if (isIdentity(t, sourceWidth, sourceHeight, profile))
        return filters;
    auto number = [](double value) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.4f", value);
        std::string text = buf;
        while (text.back() == '0')
            text.pop_back();
        if (text.back() == '.')
            text.pop_back();
        return text;
    };
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
    const double x = (p.cx - p.w / 2) * outputScale, y = (p.cy - p.h / 2) * outputScale;
    // use_normalized: the picture is drawn on a frame-sized canvas, whatever
    // size the compositor asks for, with the (cropped) source stretched to
    // it, so `rect` is exactly where the picture goes. Without it the
    // canvas was the source's own size and every placement came out scaled
    // by source/frame. repeat_off/mirror_off: affine otherwise tiles and
    // mirrors the picture outside `rect` (as kdenlive's pan_zoom sets them).
    // All verified with standalone repros (MLT 7.40, 2026-09-25).
    filters.push_back({"affine",
                       {{"use_normalized", "1"},
                        {"transition.rect", number(x) + " " + number(y) + " " + number(p.w * outputScale) + " " +
                                                number(p.h * outputScale) + " 1"},
                        {"transition.distort", "1"},
                        {"transition.repeat_off", "1"},
                        {"transition.mirror_off", "1"},
                        {"transition.fix_rotate_x", number(p.rotation)}}});
    return filters;
}

} // namespace ustudio::core
