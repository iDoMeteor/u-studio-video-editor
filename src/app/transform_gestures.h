#pragma once

// M4 F2 (ADR-018): the preview's transform handles as plain arithmetic, no
// GTK, so every gesture is tested without a window (tests/app/
// test_transform_gestures). The overlay (transform_overlay.cpp) turns
// pointer events into these calls and the results into SetClipTransform.
//
// Coordinates are the frame's (project pixels, (0, 0) top left, y down);
// sizes that are about the pointer (handle radii, snap distance) are given
// in widget pixels with the mapping's scale (widget pixels per frame
// pixel), so the handles stay the same size at any preview size.
// Rotation is degrees clockwise on screen, as Transform::rotation.

#include "core/model/model.h"
#include "core/model/transform.h"

#include <array>
#include <optional>
#include <vector>

namespace ustudio::app::gestures {

struct Point
{
    double x = 0, y = 0;
};

// The picture's corners: top left, top right, bottom right, bottom left.
std::array<Point, 4> corners(const core::Placement &p);
bool contains(const core::Placement &p, Point frame);
// The axis-aligned box around the (rotated) picture: left, top, right, bottom.
std::array<double, 4> boundingBox(const core::Placement &p);

// A clip's picture as the preview shows it at a frame.
struct VisibleClip
{
    core::ClipId clip;
    core::Placement placement;
};
// Every video clip showing at `frame`, topmost first: visible video tracks,
// video enabled, covering the frame.
std::vector<VisibleClip> visibleClips(const core::Model &model, core::FrameIndex frame);
// The topmost of them whose picture contains `point`; none if no picture does.
std::optional<VisibleClip> clipAt(const core::Model &model, core::FrameIndex frame, Point point);
// Where `clip`'s picture is (its asset's size under its transform).
core::Placement placementOf(const core::Model &model, core::ClipId clip);

enum class Handle
{
    None,
    Body, // inside the picture: move
    TopLeft,
    Top,
    TopRight,
    Right,
    BottomRight,
    Bottom,
    BottomLeft,
    Left,
    Rotate, // the knob above the top edge
};

constexpr double kHandleRadius = 7.0;  // widget pixels: grab distance and drawn half-size
constexpr double kRotateOffset = 28.0; // widget pixels from the top edge to the rotate knob
constexpr double kSnapDistance = 8.0;  // widget pixels

// Where a handle sits, in frame pixels; `scale` is widget pixels per frame
// pixel (PreviewMapping::scale()).
Point handlePosition(const core::Placement &p, Handle handle, double scale);
// Which handle `point` grabs: the rotate knob, then corners, then edges,
// then the picture itself; None outside all of them.
Handle hitTest(const core::Placement &p, Point point, double scale);

struct Modifiers
{
    bool shift = false;   // corners: free the aspect; rotate: 15-degree steps
    bool control = false; // no snapping
    bool alt = false;     // edges and corners crop instead of scaling
};

// Lines the picture may snap to: the frame's edges and centre lines and
// every other visible picture's box edges and centre.
struct SnapTargets
{
    std::vector<double> xs, ys;
};
SnapTargets snapTargets(const core::Model &model, core::FrameIndex frame, core::ClipId exclude);

// A snapped-to line, drawn while dragging.
struct Guide
{
    bool vertical = true;
    double position = 0;
};

struct DragStart
{
    core::Transform transform; // explicit (Bounds::None): explicitTransform() of the clip's
    int sourceWidth = 0, sourceHeight = 0;
    Handle handle = Handle::None;
    Point pointer; // where the drag began, frame pixels
};
struct DragResult
{
    core::Transform transform;
    std::vector<Guide> guides;
};
// The transform with the pointer at `now`. `snapDistance` is in frame
// pixels (kSnapDistance / scale).
DragResult drag(const DragStart &start, Point now, Modifiers modifiers, const SnapTargets &targets,
                double snapDistance);

// The explicit transform moved by (dx, dy) frame pixels.
core::Transform nudged(const core::Transform &explicitTransform, double dx, double dy);

} // namespace ustudio::app::gestures
