#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/timeline/timeline_controller.h"
#include "core/commands/undo_stack.h"
#include "core/model/model.h"

using namespace ustudio::core;
using namespace ustudio::app::timeline;

namespace {

// V1 (row 0): a [0,100) touching b [100,200), gap, c [300,400).
// V2 (row 1): d [500,600).
// Viewport: 1 px per frame, frame 0 at x = 22 (the handle strip), rows 60
// px tall with a 14 px name strip, so a clip body is at y = row*60 + 30.
struct Fixture
{
    Model model = Model::createEmpty();
    UndoStack undo{model};
    Viewport viewport;
    TrackId v1, v2;
    ClipId a, b, c, d;
    TimelineController controller;
    FrameIndex playhead = 0;

    Fixture()
    {
        v1 = model.addTrack(Track::Kind::Video, 0, "V1");
        v2 = model.addTrack(Track::Kind::Video, 1, "V2");
        Asset asset;
        asset.path = "color:red";
        asset.info.hasVideo = true;
        asset.info.lengthInSequenceFrames = 1'000'000;
        AssetId id = model.addAsset(asset);
        a = model.insertClip(v1, id, 0, 1000, 1099);
        b = model.insertClip(v1, id, 100, 2000, 2099);
        c = model.insertClip(v1, id, 300, 3000, 3099);
        d = model.insertClip(v2, id, 500, 4000, 4099);
        viewport.setOriginX(22.0);
        viewport.setVisibleWidth(1100.0);
        viewport.setSequenceLength(1000, 1);
        REQUIRE(viewport.pxPerFrame() == doctest::Approx(1.0));
    }

    TimelineContext ctx() const
    {
        return TimelineContext{.model = model,
                               .viewport = viewport,
                               .layout = RowLayout{},
                               .handleWidth = 22.0,
                               .edgeGrabPx = 8.0,
                               .dragThresholdPx = 3.0,
                               .playhead = playhead,
                               .sequenceLength = 1000};
    }
    static double x(FrameIndex frame)
    {
        return 22.0 + static_cast<double>(frame);
    }
    static double bodyY(int row)
    {
        return row * 60.0 + 30.0;
    }

    TimelineOutcome drag(double x0, double y0, double x1, double y1, Modifiers mods = Modifiers::None)
    {
        controller.press(ctx(), x0, y0, mods);
        controller.motion(ctx(), (x1 - x0) / 2, (y1 - y0) / 2);
        controller.motion(ctx(), x1 - x0, y1 - y0);
        return controller.release(ctx(), x1 - x0, y1 - y0);
    }
    // What the window does with an outcome: the first attempt that runs wins.
    int apply(TimelineOutcome &out)
    {
        for (size_t i = 0; i < out.attempts.size(); ++i) {
            if (undo.execute(std::move(out.attempts[i].command)))
                return static_cast<int>(i);
        }
        return -1;
    }
};

} // namespace

TEST_CASE("TimelineController: a click selects the clip under it and seeks there")
{
    Fixture f;
    TimelineOutcome out = f.drag(Fixture::x(150), Fixture::bodyY(0), Fixture::x(150), Fixture::bodyY(0));
    CHECK(out.attempts.empty());
    REQUIRE(out.seek);
    CHECK(*out.seek == 150);
    CHECK(out.activeRow == 0);
    CHECK(f.controller.selection().single() == f.b);

    out = f.drag(Fixture::x(250), Fixture::bodyY(0), Fixture::x(251), Fixture::bodyY(0));
    CHECK(f.controller.selection().empty());
    CHECK(*out.seek == 250);
}

TEST_CASE("TimelineController: the handle strip only makes the row active")
{
    Fixture f;
    TimelineOutcome out = f.controller.click(f.ctx(), 1, 10.0, Fixture::bodyY(1), Modifiers::None);
    CHECK(out.activeRow == 1);
    CHECK_FALSE(out.seek);
}

TEST_CASE("TimelineController: double-click renames the track on its name strip, else the clip")
{
    Fixture f;
    TimelineOutcome out = f.controller.click(f.ctx(), 2, Fixture::x(50), 5.0, Modifiers::None);
    CHECK(out.rename == TimelineOutcome::Rename::Track);
    CHECK(out.renameRow == 0);
    out = f.controller.click(f.ctx(), 2, Fixture::x(50), Fixture::bodyY(0), Modifiers::None);
    CHECK(out.rename == TimelineOutcome::Rename::Clip);
    CHECK(out.renameClip == f.a);
}

TEST_CASE("TimelineController: dragging a clip body moves it, across tracks too")
{
    Fixture f;
    TimelineOutcome out = f.drag(Fixture::x(350), Fixture::bodyY(0), Fixture::x(450), Fixture::bodyY(0));
    REQUIRE(out.attempts.size() == 1);
    CHECK(f.apply(out) == 0);
    CHECK(f.model.clip(f.c).position == 400);

    out = f.drag(Fixture::x(450), Fixture::bodyY(0), Fixture::x(450), Fixture::bodyY(1));
    CHECK(f.apply(out) == 0);
    CHECK(f.model.clip(f.c).track == f.v2);
    CHECK(out.activeRowOnSuccess == 1);
}

TEST_CASE("TimelineController: a moved clip snaps to a nearby edge, never to its own")
{
    Fixture f;
    // c starts at 300; drop it 5 px from b's end at 200 on the same track:
    // its start snaps to 200.
    TimelineOutcome out = f.drag(Fixture::x(350), Fixture::bodyY(0), Fixture::x(255), Fixture::bodyY(0));
    CHECK(f.apply(out) == 0);
    CHECK(f.model.clip(f.c).position == 200);

    // A 4 px nudge away from where it started doesn't snap back to its own
    // old start.
    Fixture g;
    TimelineOutcome back = g.drag(Fixture::x(350), Fixture::bodyY(0), Fixture::x(354), Fixture::bodyY(0));
    CHECK(g.apply(back) == 0);
    CHECK(g.model.clip(g.c).position == 304);
}

TEST_CASE("TimelineController: snapping takes edges on other tracks and the playhead")
{
    Fixture f;
    f.playhead = 700;
    // d [500,600) on V2: its end dragged to 695 snaps to the playhead.
    TimelineOutcome out = f.drag(Fixture::x(550), Fixture::bodyY(1), Fixture::x(645), Fixture::bodyY(1));
    CHECK(f.apply(out) == 0);
    CHECK(f.model.clip(f.d).end() == 700);

    // a clip on V2 snapping to c's start on V1
    Fixture g;
    TimelineOutcome cross = g.drag(Fixture::x(550), Fixture::bodyY(1), Fixture::x(353), Fixture::bodyY(1));
    CHECK(g.apply(cross) == 0);
    CHECK(g.model.clip(g.d).position == 300);
}

TEST_CASE("TimelineController: dropping a clip back where it was issues nothing")
{
    Fixture f;
    TimelineOutcome out = f.drag(Fixture::x(350), Fixture::bodyY(0), Fixture::x(350), Fixture::bodyY(0) + 10);
    CHECK(out.attempts.empty());
}

TEST_CASE("TimelineController: a refused move reports why")
{
    Fixture f;
    // c onto b: occupied.
    TimelineOutcome out = f.drag(Fixture::x(350), Fixture::bodyY(0), Fixture::x(170), Fixture::bodyY(0));
    CHECK(f.apply(out) == -1);
    CHECK(out.failureStatus.find("occupied") != std::string::npos);
    CHECK(f.model.clip(f.c).position == 300);
}

TEST_CASE("TimelineController: trimming a clip's edges")
{
    Fixture f;
    // c's tail from 400 back to 380
    TimelineOutcome out = f.drag(Fixture::x(399), Fixture::bodyY(0), Fixture::x(379), Fixture::bodyY(0));
    CHECK(f.apply(out) == 0);
    CHECK(f.model.clip(f.c).end() == 380);
    // c's head from 300 on to 320
    out = f.drag(Fixture::x(301), Fixture::bodyY(0), Fixture::x(321), Fixture::bodyY(0));
    CHECK(f.apply(out) == 0);
    CHECK(f.model.clip(f.c).position == 320);
    CHECK(f.model.clip(f.c).in == 3020);
}

TEST_CASE("TimelineController: trimming into a touching neighbour makes a dissolve")
{
    Fixture f;
    // a's tail dragged 10 frames into b
    TimelineOutcome out = f.drag(Fixture::x(99), Fixture::bodyY(0), Fixture::x(109), Fixture::bodyY(0));
    REQUIRE(out.attempts.size() == 2);
    CHECK(f.apply(out) == 1);
    REQUIRE(f.model.sequence().transitions.size() == 1);
    CHECK(f.model.sequence().transitions[0].extendA == 10);
    CHECK(out.attempts[1].successStatus == "Created a 10-frame dissolve.");
}

TEST_CASE("TimelineController: dragging a dissolve's edge resizes it; to nothing removes it")
{
    Fixture f;
    TransitionId t = f.model.addTransition(f.v1, f.a, f.b, 5, 5); // overlap [95,105)
    REQUIRE(f.model.clip(f.b).position == 95);
    // Right edge (a's end, 105) out to 115: extendA 5 -> 15.
    TimelineOutcome out = f.drag(Fixture::x(105), Fixture::bodyY(0), Fixture::x(115), Fixture::bodyY(0));
    CHECK(f.controller.mode() == TimelineController::Mode::None);
    CHECK(f.apply(out) == 0);
    REQUIRE(f.model.sequence().transitions.size() == 1);
    CHECK(f.model.sequence().transitions[0].extendA == 15);
    CHECK(f.model.sequence().transitions[0].extendB == 5);
    (void)t;
}

TEST_CASE("TimelineController: the handle strip drags tracks")
{
    Fixture f;
    TimelineOutcome out = f.drag(10.0, Fixture::bodyY(0), 10.0, Fixture::bodyY(1));
    CHECK(f.apply(out) == 0);
    CHECK(f.model.sequence().tracks[1].id == f.v1);
    CHECK(out.activeRowOnSuccess == 1);
}

TEST_CASE("TimelineController: empty space scrubs")
{
    Fixture f;
    TimelineOutcome down = f.controller.press(f.ctx(), Fixture::x(250), Fixture::bodyY(0), Modifiers::None);
    CHECK(*down.seek == 250);
    TimelineOutcome moved = f.controller.motion(f.ctx(), 30.0, 0.0);
    CHECK(*moved.seek == 280);
    TimelineOutcome up = f.controller.release(f.ctx(), 30.0, 0.0);
    CHECK(up.attempts.empty());
}

TEST_CASE("TimelineController: the drag preview follows the pointer for drawing")
{
    Fixture f;
    f.controller.press(f.ctx(), Fixture::x(350), Fixture::bodyY(0), Modifiers::None);
    CHECK(f.controller.mode() == TimelineController::Mode::MoveClip);
    f.controller.motion(f.ctx(), 40.0, 60.0);
    CHECK(f.controller.preview().clip == f.c);
    CHECK(f.controller.preview().row == 1);
    CHECK(f.controller.preview().start == 340);
    f.controller.cancel();
    CHECK(f.controller.mode() == TimelineController::Mode::None);
}

TEST_CASE("TimelineController: what a right-click lands on")
{
    Fixture f;
    ContextTarget onClip = f.controller.contextTargetAt(f.ctx(), Fixture::x(350), Fixture::bodyY(0));
    CHECK(onClip.clip == f.c);
    CHECK_FALSE(onClip.gapStart);

    ContextTarget inGap = f.controller.contextTargetAt(f.ctx(), Fixture::x(250), Fixture::bodyY(0));
    CHECK_FALSE(inGap.clip.isValid());
    REQUIRE(inGap.gapStart);
    CHECK(*inGap.gapStart == 200);

    ContextTarget pastEnd = f.controller.contextTargetAt(f.ctx(), Fixture::x(800), Fixture::bodyY(0));
    CHECK_FALSE(pastEnd.gapStart);

    ContextTarget atCut = f.controller.contextTargetAt(f.ctx(), Fixture::x(100) + 3, Fixture::bodyY(0));
    CHECK(atCut.addTransitionA == f.a);
    CHECK(atCut.addTransitionB == f.b);

    f.model.addTransition(f.v1, f.a, f.b, 5, 5);
    ContextTarget inDissolve = f.controller.contextTargetAt(f.ctx(), Fixture::x(100), Fixture::bodyY(0));
    CHECK(inDissolve.transition.isValid());
    CHECK_FALSE(inDissolve.addTransitionA.isValid());
}
