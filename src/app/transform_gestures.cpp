#include "transform_gestures.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ustudio::app::gestures {

namespace {

constexpr double kRadiansPerDegree = std::numbers::pi / 180.0;
constexpr double kRightAngleSnap = 3.0; // degrees: rotation snaps to 0/90/180/270 this close

// A vector in the picture's own axes turned onto the frame's, and back.
Point toFrame(const core::Placement &p, double lx, double ly)
{
    const double c = std::cos(p.rotation * kRadiansPerDegree), s = std::sin(p.rotation * kRadiansPerDegree);
    return {lx * c - ly * s, lx * s + ly * c};
}

Point toLocal(const core::Placement &p, double fx, double fy)
{
    const double c = std::cos(p.rotation * kRadiansPerDegree), s = std::sin(p.rotation * kRadiansPerDegree);
    return {fx * c + fy * s, -fx * s + fy * c};
}

// The handle's side on each axis: -1 left/top, +1 right/bottom, 0 neither.
std::pair<int, int> sides(Handle handle)
{
    switch (handle) {
    case Handle::TopLeft:
        return {-1, -1};
    case Handle::Top:
        return {0, -1};
    case Handle::TopRight:
        return {1, -1};
    case Handle::Right:
        return {1, 0};
    case Handle::BottomRight:
        return {1, 1};
    case Handle::Bottom:
        return {0, 1};
    case Handle::BottomLeft:
        return {-1, 1};
    case Handle::Left:
        return {-1, 0};
    default:
        return {0, 0};
    }
}

bool axisAligned(double rotation)
{
    const double r = std::fmod(std::abs(rotation), 360.0);
    return r < 1e-6 || std::abs(r - 360.0) < 1e-6;
}

double normalisedDegrees(double degrees)
{
    double r = std::fmod(degrees, 360.0);
    if (r > 180.0)
        r -= 360.0;
    else if (r <= -180.0)
        r += 360.0;
    return r;
}

// The smallest shift within `distance` that puts one of `edges` on one of
// `targets`, and the target it lands on.
std::optional<std::pair<double, double>> snapShift(const std::vector<double> &edges, const std::vector<double> &targets,
                                                   double distance)
{
    std::optional<std::pair<double, double>> best;
    for (double edge : edges)
        for (double target : targets) {
            const double shift = target - edge;
            if (std::abs(shift) <= distance && (!best || std::abs(shift) < std::abs(best->first)))
                best = std::pair{shift, target};
        }
    return best;
}

core::Placement placementOfTransform(const core::Transform &t)
{
    return {t.x.value, t.y.value, t.width.value, t.height.value, t.rotation.value};
}

} // namespace

std::array<Point, 4> corners(const core::Placement &p)
{
    std::array<Point, 4> out;
    const double hw = p.w / 2, hh = p.h / 2;
    const double local[4][2] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
    for (size_t i = 0; i < 4; ++i) {
        const Point v = toFrame(p, local[i][0], local[i][1]);
        out[i] = {p.cx + v.x, p.cy + v.y};
    }
    return out;
}

bool contains(const core::Placement &p, Point frame)
{
    const Point local = toLocal(p, frame.x - p.cx, frame.y - p.cy);
    return std::abs(local.x) <= p.w / 2 && std::abs(local.y) <= p.h / 2;
}

std::array<double, 4> boundingBox(const core::Placement &p)
{
    const std::array<Point, 4> c = corners(p);
    std::array<double, 4> box{c[0].x, c[0].y, c[0].x, c[0].y};
    for (const Point &point : c) {
        box[0] = std::min(box[0], point.x);
        box[1] = std::min(box[1], point.y);
        box[2] = std::max(box[2], point.x);
        box[3] = std::max(box[3], point.y);
    }
    return box;
}

core::Placement placementOf(const core::Model &model, core::ClipId clipId, core::FrameIndex frame)
{
    const core::Clip &clip = model.clip(clipId);
    int width = 0, height = 0;
    if (model.hasAsset(clip.asset)) {
        width = model.asset(clip.asset).info.width;
        height = model.asset(clip.asset).info.height;
    }
    return core::placementFor(transformShownAt(model, clipId, frame), width, height, model.sequence().profile);
}

core::Transform transformShownAt(const core::Model &model, core::ClipId clipId, core::FrameIndex frame)
{
    const core::Clip &clip = model.clip(clipId);
    return core::transformAt(clip.transform.get(), frame - clip.position);
}

core::Transform transformEditedAt(const core::Model &model, core::ClipId clipId, core::FrameIndex frame,
                                  const core::Transform &edited)
{
    const core::Clip &clip = model.clip(clipId);
    const core::Transform &current = clip.transform.get();
    if (!core::isAnimated(current) || edited.bounds != core::Transform::Bounds::None)
        return edited;
    return core::withTransformAt(current, frame - clip.position, edited);
}

std::vector<VisibleClip> visibleClips(const core::Model &model, core::FrameIndex frame)
{
    std::vector<VisibleClip> out;
    for (const core::Track &track : model.sequence().tracks) { // index 0 is the top
        if (track.kind != core::Track::Kind::Video || track.hidden)
            continue;
        for (core::ClipId id : track.clips) {
            const core::Clip &clip = model.clip(id);
            if (clip.position > frame)
                break; // sorted by position
            if (frame < clip.end() && clip.videoEnabled)
                out.push_back({id, placementOf(model, id, frame)});
        }
    }
    return out;
}

std::optional<VisibleClip> clipAt(const core::Model &model, core::FrameIndex frame, Point point)
{
    for (const VisibleClip &visible : visibleClips(model, frame))
        if (contains(visible.placement, point))
            return visible;
    return std::nullopt;
}

Point handlePosition(const core::Placement &p, Handle handle, double scale)
{
    double lx = 0, ly = 0;
    if (handle == Handle::Rotate) {
        ly = -p.h / 2 - kRotateOffset / std::max(scale, 1e-9);
    } else {
        const auto [sx, sy] = sides(handle);
        lx = sx * p.w / 2;
        ly = sy * p.h / 2;
    }
    const Point v = toFrame(p, lx, ly);
    return {p.cx + v.x, p.cy + v.y};
}

Handle hitTest(const core::Placement &p, Point point, double scale)
{
    auto near = [&](Handle handle, double radius) {
        const Point h = handlePosition(p, handle, scale);
        return std::hypot(point.x - h.x, point.y - h.y) * scale <= radius;
    };
    if (near(Handle::Rotate, kHandleRadius + 2))
        return Handle::Rotate;
    for (Handle handle : {Handle::TopLeft, Handle::TopRight, Handle::BottomRight, Handle::BottomLeft})
        if (near(handle, kHandleRadius + 2))
            return handle;
    for (Handle handle : {Handle::Top, Handle::Right, Handle::Bottom, Handle::Left})
        if (near(handle, kHandleRadius + 2))
            return handle;
    return contains(p, point) ? Handle::Body : Handle::None;
}

SnapTargets snapTargets(const core::Model &model, core::FrameIndex frame, core::ClipId exclude)
{
    const core::Profile &profile = model.sequence().profile;
    SnapTargets targets{{0.0, profile.width / 2.0, static_cast<double>(profile.width)},
                        {0.0, profile.height / 2.0, static_cast<double>(profile.height)}};
    for (const VisibleClip &visible : visibleClips(model, frame)) {
        if (visible.clip == exclude)
            continue;
        const std::array<double, 4> box = boundingBox(visible.placement);
        targets.xs.insert(targets.xs.end(), {box[0], (box[0] + box[2]) / 2, box[2]});
        targets.ys.insert(targets.ys.end(), {box[1], (box[1] + box[3]) / 2, box[3]});
    }
    return targets;
}

DragResult drag(const DragStart &start, Point now, Modifiers modifiers, const SnapTargets &targets, double snapDistance)
{
    DragResult result{start.transform, {}};
    core::Transform &t = result.transform;
    const core::Placement p0 = placementOfTransform(start.transform);
    const double dx = now.x - start.pointer.x, dy = now.y - start.pointer.y;
    const bool snap = !modifiers.control;

    if (start.handle == Handle::Body) {
        t.x.value = p0.cx + dx;
        t.y.value = p0.cy + dy;
        if (snap) {
            const std::array<double, 4> box = boundingBox(placementOfTransform(t));
            if (auto s = snapShift({box[0], (box[0] + box[2]) / 2, box[2]}, targets.xs, snapDistance)) {
                t.x.value += s->first;
                result.guides.push_back({true, s->second});
            }
            if (auto s = snapShift({box[1], (box[1] + box[3]) / 2, box[3]}, targets.ys, snapDistance)) {
                t.y.value += s->first;
                result.guides.push_back({false, s->second});
            }
        }
        return result;
    }

    if (start.handle == Handle::Rotate) {
        const double a0 = std::atan2(start.pointer.y - p0.cy, start.pointer.x - p0.cx);
        const double a1 = std::atan2(now.y - p0.cy, now.x - p0.cx);
        double rotation = normalisedDegrees(p0.rotation + (a1 - a0) / kRadiansPerDegree);
        if (modifiers.shift) {
            rotation = normalisedDegrees(std::round(rotation / 15.0) * 15.0);
        } else if (snap) {
            const double right = std::round(rotation / 90.0) * 90.0;
            if (std::abs(rotation - right) <= kRightAngleSnap)
                rotation = normalisedDegrees(right);
        }
        t.rotation.value = rotation;
        return result;
    }

    const auto [sx, sy] = sides(start.handle);
    if (sx == 0 && sy == 0)
        return result;
    const Point local = toLocal(p0, dx, dy);
    double nw = p0.w, nh = p0.h;

    if (modifiers.alt) {
        // Crop: the far edge and the picture under it stay put. Source
        // pixels per screen pixel from the start; crop edges are the
        // source's, so a flipped picture's screen right is its source left.
        const int srcW = start.sourceWidth > 0 ? start.sourceWidth : static_cast<int>(std::lround(p0.w));
        const int srcH = start.sourceHeight > 0 ? start.sourceHeight : static_cast<int>(std::lround(p0.h));
        const double perSourceX = p0.w / core::croppedWidth(start.transform, srcW);
        const double perSourceY = p0.h / core::croppedHeight(start.transform, srcH);
        if (sx != 0) {
            core::KeyframedValue &crop = (sx > 0) != start.transform.flipH ? t.cropRight : t.cropLeft;
            const double other = (&crop == &t.cropRight ? t.cropLeft : t.cropRight).value;
            const double wanted = crop.value - sx * local.x / perSourceX;
            const double cropped = std::clamp(wanted, 0.0, std::max(0.0, srcW - other - 1.0));
            nw = p0.w + (crop.value - cropped) * perSourceX;
            crop.value = cropped;
        }
        if (sy != 0) {
            core::KeyframedValue &crop = (sy > 0) != start.transform.flipV ? t.cropBottom : t.cropTop;
            const double other = (&crop == &t.cropBottom ? t.cropTop : t.cropBottom).value;
            const double wanted = crop.value - sy * local.y / perSourceY;
            const double cropped = std::clamp(wanted, 0.0, std::max(0.0, srcH - other - 1.0));
            nh = p0.h + (crop.value - cropped) * perSourceY;
            crop.value = cropped;
        }
    } else {
        const bool corner = sx != 0 && sy != 0;
        const bool keepAspect = corner && !modifiers.shift;
        if (sx != 0)
            nw = p0.w + sx * local.x;
        if (sy != 0)
            nh = p0.h + sy * local.y;
        if (keepAspect) {
            // The pointer's move along the diagonal: equal relative growth.
            const double s = std::max(1.0 + (sx * local.x / p0.w + sy * local.y / p0.h) / 2.0, 1.0 / p0.w);
            nw = p0.w * s;
            nh = p0.h * s;
        }
        nw = std::max(nw, 1.0);
        nh = std::max(nh, 1.0);
        if (snap && axisAligned(p0.rotation)) {
            // The moving edges, with the opposite ones fixed.
            const double fixedX = p0.cx - sx * p0.w / 2, fixedY = p0.cy - sy * p0.h / 2;
            std::optional<std::pair<double, double>> snapX, snapY;
            if (sx != 0)
                snapX = snapShift({fixedX + sx * nw}, targets.xs, snapDistance);
            if (sy != 0)
                snapY = snapShift({fixedY + sy * nh}, targets.ys, snapDistance);
            if (keepAspect && snapX && snapY) {
                // One axis decides the size: the closer snap.
                if (std::abs(snapX->first) <= std::abs(snapY->first))
                    snapY.reset();
                else
                    snapX.reset();
            }
            if (snapX) {
                nw = std::max(1.0, nw + sx * snapX->first);
                if (keepAspect)
                    nh = nw * p0.h / p0.w;
                result.guides.push_back({true, snapX->second});
            }
            if (snapY) {
                nh = std::max(1.0, nh + sy * snapY->first);
                if (keepAspect)
                    nw = nh * p0.w / p0.h;
                result.guides.push_back({false, snapY->second});
            }
        }
    }
    // The opposite side stays where it was.
    const Point shift = toFrame(p0, sx * (nw - p0.w) / 2, sy * (nh - p0.h) / 2);
    t.x.value = p0.cx + shift.x;
    t.y.value = p0.cy + shift.y;
    t.width.value = nw;
    t.height.value = nh;
    return result;
}

core::Transform nudged(const core::Transform &explicitTransform, double dx, double dy)
{
    core::Transform t = explicitTransform;
    t.x.value += dx;
    t.y.value += dy;
    return t;
}

} // namespace ustudio::app::gestures
