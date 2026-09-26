// M4 F2 (ADR-018): the preview's transform handles, without a window.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/transform_gestures.h"

#include <algorithm>

using namespace ustudio::core;
using namespace ustudio::app::gestures;

namespace {

Transform placed(double cx, double cy, double w, double h, double rotation = 0)
{
    Transform t;
    t.bounds = Transform::Bounds::None;
    t.x.value = cx;
    t.y.value = cy;
    t.width.value = w;
    t.height.value = h;
    t.rotation.value = rotation;
    return t;
}

// V2 above V1, each with one 1080p clip over frames 0-99.
struct Scene
{
    Model model = Model::createEmpty();
    TrackId top, bottom;
    ClipId upper, lower;

    Scene()
    {
        bottom = model.addTrack(Track::Kind::Video, 0, "V1");
        top = model.addTrack(Track::Kind::Video, 0, "V2"); // index 0: the top
        Asset asset;
        asset.path = "color:red";
        asset.info.hasVideo = true;
        asset.info.width = 1920;
        asset.info.height = 1080;
        asset.info.lengthInSequenceFrames = 300;
        const AssetId id = model.addAsset(asset);
        lower = model.insertClip(bottom, id, 0, 0, 99);
        upper = model.insertClip(top, id, 0, 0, 99);
    }
};

DragStart startOf(const Transform &t, Handle handle, Point pointer)
{
    return {t, 1920, 1080, handle, pointer};
}

const SnapTargets kNoTargets;

} // namespace

TEST_CASE("gestures: the topmost visible picture under the pointer is the one picked")
{
    Scene scene;
    scene.model.setClipTransform(scene.upper, placed(480, 270, 960, 540)); // the top-left quarter
    auto picked = clipAt(scene.model, 10, {100, 100});
    REQUIRE(picked);
    CHECK(picked->clip == scene.upper);
    CHECK(picked->placement.w == doctest::Approx(960));
    picked = clipAt(scene.model, 10, {1500, 900}); // beside the upper picture: the full-frame lower one
    REQUIRE(picked);
    CHECK(picked->clip == scene.lower);
    CHECK_FALSE(clipAt(scene.model, 150, {100, 100})); // after both clips end
    CHECK_FALSE(clipAt(scene.model, 10, {-50, 100}));  // outside every picture

    scene.model.setTrackFlags(scene.top, false, true, false); // a hidden track shows nothing
    picked = clipAt(scene.model, 10, {100, 100});
    REQUIRE(picked);
    CHECK(picked->clip == scene.lower);
    CHECK(visibleClips(scene.model, 10).size() == 1);
}

TEST_CASE("gestures: handles are hit within their radius in widget pixels")
{
    const Placement p{960, 540, 960, 540, 0};
    const double scale = 0.5; // a 960-wide preview of a 1920 frame
    CHECK(hitTest(p, {480, 270}, scale) == Handle::TopLeft);
    CHECK(hitTest(p, {490, 280}, scale) == Handle::TopLeft); // 7 widget px away
    CHECK(hitTest(p, {1440, 540}, scale) == Handle::Right);
    CHECK(hitTest(p, {960, 810}, scale) == Handle::Bottom);
    const Point knob = handlePosition(p, Handle::Rotate, scale);
    CHECK(knob.x == doctest::Approx(960));
    CHECK(knob.y == doctest::Approx(270 - kRotateOffset / scale));
    CHECK(hitTest(p, knob, scale) == Handle::Rotate);
    CHECK(hitTest(p, {700, 600}, scale) == Handle::Body);
    CHECK(hitTest(p, {100, 100}, scale) == Handle::None);

    // Rotated 90 degrees clockwise: the picture's top edge faces right.
    const Placement turned{960, 540, 960, 540, 90};
    CHECK(hitTest(turned, {960 + 270, 540}, scale) == Handle::Top);
    CHECK(hitTest(turned, {960 - 270, 540 - 480}, scale) == Handle::BottomLeft);
}

TEST_CASE("gestures: moving snaps to the frame and other pictures, Ctrl bypasses it")
{
    const Transform t = placed(960, 540, 960, 540);
    DragResult r = drag(startOf(t, Handle::Body, {960, 540}), {1060, 590}, {}, kNoTargets, 16);
    CHECK(r.transform.x.value == doctest::Approx(1060));
    CHECK(r.transform.y.value == doctest::Approx(590));
    CHECK(r.guides.empty());

    Scene scene;
    scene.model.setClipTransform(scene.upper, t);
    scene.model.setTrackFlags(scene.bottom, false, true, false);
    const SnapTargets frame = snapTargets(scene.model, 10, scene.upper);
    CHECK(frame.xs == std::vector<double>{0, 960, 1920});
    // Left edge 10 px from the frame's: it lands on it, with a guide.
    r = drag(startOf(t, Handle::Body, {960, 540}), {960 - 470, 545}, {}, frame, 16);
    CHECK(r.transform.x.value == doctest::Approx(480));
    CHECK(r.transform.y.value == doctest::Approx(540)); // and the centre line
    REQUIRE(r.guides.size() == 2);
    CHECK(r.guides[0].vertical);
    CHECK(r.guides[0].position == doctest::Approx(0));
    Modifiers ctrl;
    ctrl.control = true;
    r = drag(startOf(t, Handle::Body, {960, 540}), {960 - 470, 545}, ctrl, frame, 16);
    CHECK(r.transform.x.value == doctest::Approx(490));
    CHECK(r.guides.empty());

    // Another visible picture's edges and centre are targets too.
    scene.model.setTrackFlags(scene.bottom, false, false, false);
    scene.model.setClipTransform(scene.lower, placed(1600, 300, 400, 200));
    const SnapTargets both = snapTargets(scene.model, 10, scene.upper);
    CHECK(std::ranges::find(both.xs, 1400.0) != both.xs.end());
    CHECK(std::ranges::find(both.ys, 400.0) != both.ys.end());
}

TEST_CASE("gestures: a corner keeps the aspect, Shift frees it, an edge stretches one side")
{
    const Transform t = placed(960, 540, 960, 540); // top left at (480, 270)
    DragResult r = drag(startOf(t, Handle::BottomRight, {1440, 810}), {1440 + 96, 810 + 54}, {}, kNoTargets, 16);
    CHECK(r.transform.width.value == doctest::Approx(1056));
    CHECK(r.transform.height.value == doctest::Approx(594));
    CHECK(r.transform.x.value - r.transform.width.value / 2 == doctest::Approx(480)); // the far corner stays
    CHECK(r.transform.y.value - r.transform.height.value / 2 == doctest::Approx(270));

    Modifiers shift;
    shift.shift = true;
    r = drag(startOf(t, Handle::BottomRight, {1440, 810}), {1540, 810}, shift, kNoTargets, 16);
    CHECK(r.transform.width.value == doctest::Approx(1060));
    CHECK(r.transform.height.value == doctest::Approx(540));

    r = drag(startOf(t, Handle::Left, {480, 540}), {380, 600}, {}, kNoTargets, 16);
    CHECK(r.transform.width.value == doctest::Approx(1060));
    CHECK(r.transform.height.value == doctest::Approx(540));
    CHECK(r.transform.x.value + r.transform.width.value / 2 == doctest::Approx(1440)); // the right edge stays
    CHECK(r.transform.y.value == doctest::Approx(540));

    // A picture never shrinks below a pixel.
    r = drag(startOf(t, Handle::Right, {1440, 540}), {-5000, 540}, {}, kNoTargets, 16);
    CHECK(r.transform.width.value == doctest::Approx(1));
}

TEST_CASE("gestures: a rotated picture resizes along its own axes")
{
    const Transform t = placed(960, 540, 960, 540, 90); // its right edge faces down
    const DragResult r = drag(startOf(t, Handle::Right, {960, 540 + 480}), {960, 540 + 580}, {}, kNoTargets, 16);
    CHECK(r.transform.width.value == doctest::Approx(1060));
    CHECK(r.transform.height.value == doctest::Approx(540));
    CHECK(r.transform.x.value == doctest::Approx(960));
    CHECK(r.transform.y.value == doctest::Approx(590));
}

TEST_CASE("gestures: resizing snaps the moving edge")
{
    const Transform t = placed(960, 540, 960, 540);
    const SnapTargets frame{{0, 960, 1920}, {0, 540, 1080}};
    DragResult r = drag(startOf(t, Handle::Right, {1440, 540}), {1910, 540}, {}, frame, 16);
    CHECK(r.transform.width.value == doctest::Approx(1440)); // right edge on 1920
    REQUIRE(r.guides.size() == 1);
    CHECK(r.guides[0].position == doctest::Approx(1920));
    // A corner keeping its aspect snaps by the closer axis.
    r = drag(startOf(t, Handle::BottomRight, {1440, 810}), {1910, 1075}, {}, frame, 16);
    CHECK(r.transform.width.value / r.transform.height.value == doctest::Approx(960.0 / 540.0));
    CHECK(r.guides.size() == 1);
}

TEST_CASE("gestures: rotating follows the pointer about the centre, with steps and right-angle snaps")
{
    const Transform t = placed(960, 540, 960, 540);
    DragResult r = drag(startOf(t, Handle::Rotate, {960, 100}), {1400, 540}, {}, kNoTargets, 16);
    CHECK(r.transform.rotation.value == doctest::Approx(90));
    r = drag(startOf(t, Handle::Rotate, {960, 100}), {600, 540}, {}, kNoTargets, 16);
    CHECK(r.transform.rotation.value == doctest::Approx(-90).epsilon(0.001));

    // 88.5 degrees: snapped to 90, unless Ctrl.
    const double a = (88.5 - 90) * 3.14159265358979 / 180;
    const Point near{960 + 440 * std::cos(a), 540 + 440 * std::sin(a)};
    CHECK(drag(startOf(t, Handle::Rotate, {960, 100}), near, {}, kNoTargets, 16).transform.rotation.value ==
          doctest::Approx(90));
    Modifiers ctrl;
    ctrl.control = true;
    CHECK(drag(startOf(t, Handle::Rotate, {960, 100}), near, ctrl, kNoTargets, 16).transform.rotation.value ==
          doctest::Approx(88.5));
    Modifiers shift;
    shift.shift = true;
    const double b = (-90 + 20) * 3.14159265358979 / 180; // 20 degrees
    const Point twenty{960 + 440 * std::cos(b), 540 + 440 * std::sin(b)};
    CHECK(drag(startOf(t, Handle::Rotate, {960, 100}), twenty, shift, kNoTargets, 16).transform.rotation.value ==
          doctest::Approx(15));
}

TEST_CASE("gestures: Alt crops, keeping the picture under the far edge in place")
{
    // A 1920x1080 source shown at half size: 2 source pixels per screen pixel.
    const Transform t = placed(960, 540, 960, 540);
    Modifiers alt;
    alt.alt = true;
    DragResult r = drag(startOf(t, Handle::Right, {1440, 540}), {1344, 540}, alt, kNoTargets, 16);
    CHECK(r.transform.cropRight.value == doctest::Approx(192));
    CHECK(r.transform.width.value == doctest::Approx(864));
    CHECK(r.transform.x.value - r.transform.width.value / 2 == doctest::Approx(480));
    CHECK(r.transform.cropLeft.value == 0);

    // Dragging outward can only take back what was cropped.
    r = drag(startOf(r.transform, Handle::Right, {1344, 540}), {1600, 540}, alt, kNoTargets, 16);
    CHECK(r.transform.cropRight.value == doctest::Approx(0));
    CHECK(r.transform.width.value == doctest::Approx(960));

    // Flipped: the screen's right edge is the source's left.
    Transform flipped = t;
    flipped.flipH = true;
    r = drag(startOf(flipped, Handle::Right, {1440, 540}), {1344, 540}, alt, kNoTargets, 16);
    CHECK(r.transform.cropLeft.value == doctest::Approx(192));
    CHECK(r.transform.cropRight.value == 0);

    r = drag(startOf(t, Handle::Top, {960, 270}), {960, 5000}, alt, kNoTargets, 16); // never past the far edge
    CHECK(r.transform.cropTop.value == doctest::Approx(1079));
}

TEST_CASE("gestures: nudging moves the centre")
{
    const Transform t = nudged(placed(960, 540, 960, 540), -10, 1);
    CHECK(t.x.value == 950);
    CHECK(t.y.value == 541);
}
