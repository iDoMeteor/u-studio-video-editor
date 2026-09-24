#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/timeline/viewport.h"

using ustudio::app::timeline::Viewport;

namespace {

Viewport fitted(double width, ustudio::core::FrameIndex length)
{
    Viewport v;
    v.setOriginX(20.0);
    v.setVisibleWidth(width);
    v.setSequenceLength(length, 750);
    return v;
}

} // namespace

TEST_CASE("Viewport: fit shows the whole sequence plus 10%")
{
    Viewport v = fitted(1100.0, 1000);
    CHECK(v.fitMode());
    CHECK(v.pxPerFrame() == doctest::Approx(1.0));
    CHECK(v.xForFrame(0) == doctest::Approx(20.0));
    CHECK(v.xForFrame(1000) == doctest::Approx(1020.0));
    CHECK(v.scrollX() == 0.0);
}

TEST_CASE("Viewport: an empty or short sequence fits the minimum length instead")
{
    Viewport v = fitted(825.0, 0);
    CHECK(v.pxPerFrame() == doctest::Approx(1.0)); // 750 frames * 1.1 = 825
}

TEST_CASE("Viewport: fit mode follows the sequence and the widget until the user zooms")
{
    Viewport v = fitted(1100.0, 1000);
    v.setSequenceLength(2000, 750);
    CHECK(v.pxPerFrame() == doctest::Approx(0.5));
    v.setVisibleWidth(2200.0);
    CHECK(v.pxPerFrame() == doctest::Approx(1.0));

    v.zoomAround(20.0, 2.0);
    CHECK_FALSE(v.fitMode());
    v.setSequenceLength(4000, 750);
    CHECK(v.pxPerFrame() == doctest::Approx(2.0));

    v.fit();
    CHECK(v.fitMode());
    CHECK(v.pxPerFrame() == doctest::Approx(2200.0 / 4400.0));
}

TEST_CASE("Viewport: zooming keeps the frame under the pointer where it was")
{
    Viewport v = fitted(1100.0, 1000);
    double anchorX = 520.0; // frame 500
    REQUIRE(v.frameForX(anchorX) == 500);
    v.zoomAround(anchorX, 4.0);
    CHECK(v.pxPerFrame() == doctest::Approx(4.0));
    CHECK(v.xForFrame(500) == doctest::Approx(anchorX));
    v.zoomAround(anchorX, 0.5);
    CHECK(v.xForFrame(500) == doctest::Approx(anchorX));
}

TEST_CASE("Viewport: zoom is clamped and scroll stays inside the extent")
{
    Viewport v = fitted(1100.0, 1000);
    v.zoomAround(20.0, 1e9);
    CHECK(v.pxPerFrame() == Viewport::kMaxPxPerFrame);
    v.zoomAround(20.0, 1e-12);
    CHECK(v.pxPerFrame() == Viewport::kMinPxPerFrame);

    Viewport w = fitted(1100.0, 1000);
    w.zoomAround(20.0, 10.0); // 10 px/frame: extent 12500 px
    w.setScrollX(1e9);
    CHECK(w.scrollX() == doctest::Approx(12500.0 - 1100.0));
    w.setScrollX(-50.0);
    CHECK(w.scrollX() == 0.0);
}

TEST_CASE("Viewport: pixel and frame conversions")
{
    Viewport v = fitted(1100.0, 1000);
    v.zoomAround(20.0, 10.0);
    v.setScrollX(1000.0);
    CHECK(v.frameForX(20.0) == 100);
    CHECK(v.frameForX(24.9) == 100);
    CHECK(v.frameForX(30.0) == 101);
    CHECK(v.frameForX(-5000.0) == 0);
    CHECK(v.framesForPixels(35.0) == 3);
    CHECK(v.framesForPixels(-35.0) == -3);
    CHECK(v.firstVisibleFrame() == 100);
    CHECK(v.endVisibleFrame() == 211);
}

TEST_CASE("Viewport: following the playhead pages instead of scrolling every frame")
{
    Viewport v = fitted(1100.0, 1000);
    v.zoomAround(20.0, 10.0); // 110 frames visible
    CHECK_FALSE(v.ensureVisible(50));
    CHECK(v.ensureVisible(150));
    CHECK(v.xForFrame(150) == doctest::Approx(20.0 + 1100.0 * 0.05));
    CHECK_FALSE(v.ensureVisible(160));
    CHECK(v.ensureVisible(0));
    CHECK(v.scrollX() == 0.0);
}
