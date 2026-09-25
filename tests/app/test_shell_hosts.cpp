// Doc 15 IP5's GTK-free pieces: the row layout with drop-in lanes, the
// preview's frame-to-widget mapping, and contributed actions.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/action_registry.h"
#include "app/preview_mapping.h"
#include "app/timeline/row_layout.h"
#include "app/ui_hints.h"

#include <cstring>
#include <string>

using namespace ustudio::app;
using ustudio::app::timeline::RowLayout;

namespace {
void noop(GSimpleAction *, GVariant *, gpointer) {}
} // namespace

TEST_CASE("row layout: with no lanes every row is uniform, as before")
{
    RowLayout layout{60.0, 14.0, {}};
    for (int row = 0; row < 5; ++row) {
        CHECK(layout.rowTop(row) == doctest::Approx(row * 60.0));
        CHECK(layout.spanOf(row) == doctest::Approx(60.0));
    }
    for (double y : {0.0, 59.9, 60.0, 179.0, 300.0, -1.0})
        CHECK(layout.rowAt(y) == static_cast<int>(y / 60.0));
    CHECK(layout.contentHeight(3) == doctest::Approx(180.0));
    CHECK_FALSE(layout.inLane(70.0));
}

TEST_CASE("row layout: a lane lengthens its row and moves the rows below")
{
    RowLayout layout{60.0, 14.0, {12.0, 0.0, 20.0}};
    CHECK(layout.rowTop(1) == doctest::Approx(72.0));
    CHECK(layout.rowTop(2) == doctest::Approx(132.0));
    CHECK(layout.rowTop(3) == doctest::Approx(212.0)); // past the lanes: uniform again
    CHECK(layout.contentHeight(3) == doctest::Approx(212.0));
    CHECK(layout.rowAt(65.0) == 0); // row 0's lane
    CHECK(layout.rowAt(72.0) == 1);
    CHECK(layout.rowAt(200.0) == 2);
    CHECK(layout.rowAt(212.0 + 61.0) == 4);
    CHECK(layout.inLane(65.0));
    CHECK_FALSE(layout.inLane(59.0));
    CHECK_FALSE(layout.inLane(100.0)); // row 1 has no lane
    CHECK(layout.laneTop(2) == doctest::Approx(192.0));
    CHECK(layout.clipTop(1) == doctest::Approx(72.0 + 16.0));
}

TEST_CASE("preview mapping: letterboxed and pillarboxed frames, both ways round")
{
    // 16:9 in a wide widget: bars left and right.
    PreviewMapping wide = mapPreview(1000, 400, 1920, 1080, 16.0 / 9.0);
    CHECK(wide.imageHeight == doctest::Approx(400));
    CHECK(wide.imageWidth == doctest::Approx(400 * 16.0 / 9.0));
    CHECK(wide.imageX == doctest::Approx((1000 - 400 * 16.0 / 9.0) / 2));
    CHECK(wide.imageY == doctest::Approx(0));
    // In a tall widget: bars top and bottom.
    PreviewMapping tall = mapPreview(320, 400, 1920, 1080, 16.0 / 9.0);
    CHECK(tall.imageWidth == doctest::Approx(320));
    CHECK(tall.imageY == doctest::Approx((400 - 180) / 2.0));
    // Corners and centre, and the round trip.
    CHECK(wide.widgetX(0) == doctest::Approx(wide.imageX));
    CHECK(wide.widgetX(1920) == doctest::Approx(wide.imageX + wide.imageWidth));
    CHECK(wide.widgetY(540) == doctest::Approx(200));
    CHECK(wide.frameX(wide.widgetX(123.0)) == doctest::Approx(123.0));
    CHECK(tall.frameY(tall.widgetY(777.0)) == doctest::Approx(777.0));
    CHECK(wide.scale() == doctest::Approx(400.0 / 1080.0));
    // Nothing divides by zero.
    PreviewMapping none = mapPreview(0, 0, 0, 0, 0.0);
    CHECK(none.frameX(none.widgetX(10)) == doctest::Approx(10));
}

TEST_CASE("contributed actions: listed after the shell's; taken names and shortcuts refused")
{
    clearContributedActions();
    const size_t shell = actionSpecs().size();
    std::vector<ContributedAction> accepted =
        contributeActions({{"test-hello", "Say Hello", "Test", {"<Control><Alt>h", "space"}, &noop},
                           {"play-pause", "Mine", "Test", {}, &noop},  // the shell's name
                           {"test-hello", "Again", "Test", {}, &noop}, // taken by the first
                           {"test-nohandler", "No handler", "Test", {}, nullptr}},
                          reinterpret_cast<gpointer>(0x1));
    REQUIRE(accepted.size() == 1);
    CHECK(std::strcmp(accepted[0].spec.name, "test-hello") == 0);
    REQUIRE(accepted[0].spec.accels.size() == 1); // space is play-pause's
    CHECK(std::strcmp(accepted[0].spec.accels[0], "<Control><Alt>h") == 0);
    CHECK(accepted[0].target == reinterpret_cast<gpointer>(0x1));

    std::vector<ActionSpec> all = allActionSpecs();
    REQUIRE(all.size() == shell + 1);
    CHECK(std::strcmp(all.back().name, "test-hello") == 0);
    CHECK_FALSE(shortcutLabel("test-hello").empty());

    clearContributedActions();
    CHECK(allActionSpecs().size() == shell);
}
