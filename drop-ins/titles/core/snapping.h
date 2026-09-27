#pragma once

// Guides and snapping on the titles app's canvas (doc 16, "The titles app
// UX"): canvas edges and centre, the safe areas, thirds, and the other
// layers' edges and centres. Canvas pixels throughout; pure maths.

#include "title_document.h"

#include <optional>
#include <vector>

namespace ustudio::titles {

struct Rect
{
    double x = 0.0, y = 0.0, w = 0.0, h = 0.0;

    double right() const
    {
        return x + w;
    }
    double bottom() const
    {
        return y + h;
    }
    bool contains(double px, double py) const
    {
        return px >= x && px <= x + w && py >= y && py <= y + h;
    }
    bool operator==(const Rect &) const = default;
};

// Broadcast safe areas: action safe is 90% of the frame (5% in from each
// edge), title safe 80% (10%).
Rect actionSafe(const TitleDocument &doc);
Rect titleSafe(const TitleDocument &doc);

struct SnapGuides
{
    std::vector<double> xs, ys; // vertical lines at x, horizontal at y
};

// The canvas's own guides (edges, centre, safe areas, thirds) plus each of
// `others`' left, centre and right (top, middle, bottom).
SnapGuides snapGuides(const TitleDocument &doc, const std::vector<Rect> &others);

struct Snap
{
    double dx = 0.0, dy = 0.0;            // add to the box to snap it
    std::optional<double> guideX, guideY; // the guides it snapped to, to draw
};

// Whether canvas point (px, py) is on a layer whose box is `box`, turned
// by `rotation` degrees and scaled by `scale` about the box's centre (as
// the renderer draws it).
bool hitsBox(const Rect &box, double rotation, double scale, double px, double py);

// Snaps `box` so its nearest edge or centre lands on a guide no further
// than `threshold` away, on each axis on its own.
Snap snapBox(const Rect &box, const SnapGuides &guides, double threshold);

} // namespace ustudio::titles
