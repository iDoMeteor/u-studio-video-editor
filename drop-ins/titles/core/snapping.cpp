#include "snapping.h"

#include <cmath>
#include <numbers>

namespace ustudio::titles {

namespace {
Rect inset(const TitleDocument &doc, double fraction)
{
    const double dx = doc.width * fraction, dy = doc.height * fraction;
    return {dx, dy, doc.width - 2 * dx, doc.height - 2 * dy};
}

// The move that brings the nearest of `points` onto the nearest of
// `guides`, if within `threshold`.
void snapAxis(const double (&points)[3], const std::vector<double> &guides, double threshold, double &delta,
              std::optional<double> &guide)
{
    double best = threshold;
    for (double point : points)
        for (double line : guides) {
            const double distance = std::abs(line - point);
            if (distance <= best) {
                best = distance;
                delta = line - point;
                guide = line;
            }
        }
}
} // namespace

Rect actionSafe(const TitleDocument &doc)
{
    return inset(doc, 0.05);
}

Rect titleSafe(const TitleDocument &doc)
{
    return inset(doc, 0.10);
}

SnapGuides snapGuides(const TitleDocument &doc, const std::vector<Rect> &others)
{
    SnapGuides guides;
    const double w = doc.width, h = doc.height;
    guides.xs = {0.0, w / 3, w / 2, 2 * w / 3, w};
    guides.ys = {0.0, h / 3, h / 2, 2 * h / 3, h};
    for (const Rect &safe : {actionSafe(doc), titleSafe(doc)}) {
        guides.xs.insert(guides.xs.end(), {safe.x, safe.right()});
        guides.ys.insert(guides.ys.end(), {safe.y, safe.bottom()});
    }
    for (const Rect &other : others) {
        guides.xs.insert(guides.xs.end(), {other.x, other.x + other.w / 2, other.right()});
        guides.ys.insert(guides.ys.end(), {other.y, other.y + other.h / 2, other.bottom()});
    }
    return guides;
}

bool hitsBox(const Rect &box, double rotation, double scale, double px, double py)
{
    if (scale <= 0.0)
        return false;
    const double cx = box.x + box.w / 2, cy = box.y + box.h / 2;
    const double radians = -rotation * std::numbers::pi / 180.0;
    const double dx = px - cx, dy = py - cy;
    const double ux = (dx * std::cos(radians) - dy * std::sin(radians)) / scale;
    const double uy = (dx * std::sin(radians) + dy * std::cos(radians)) / scale;
    return box.contains(cx + ux, cy + uy);
}

Snap snapBox(const Rect &box, const SnapGuides &guides, double threshold)
{
    Snap snap;
    const double xs[3] = {box.x, box.x + box.w / 2, box.right()};
    const double ys[3] = {box.y, box.y + box.h / 2, box.bottom()};
    snapAxis(xs, guides.xs, threshold, snap.dx, snap.guideX);
    snapAxis(ys, guides.ys, threshold, snap.dy, snap.guideY);
    return snap;
}

} // namespace ustudio::titles
