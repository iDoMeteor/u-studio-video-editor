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
    bool snapping = true;

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
                               .sequenceLength = 1000,
                               .snapping = snapping};
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

TEST_CASE("TimelineController: a drop-in's top lane is no row: no active track, no context target")
{
    Fixture f;
    TimelineContext ctx = f.ctx();
    ctx.layout.topLane = 20.0;
    TimelineOutcome out = f.controller.click(ctx, 1, Fixture::x(150), 10.0, Modifiers::None);
    CHECK_FALSE(out.activeRow);
    CHECK_FALSE(out.seek);
    CHECK(f.controller.contextTargetAt(ctx, Fixture::x(150), 10.0).row == -1);
    // Below it, the rows are where they moved to.
    CHECK(f.controller.contextTargetAt(ctx, Fixture::x(150), 20.0 + Fixture::bodyY(1)).row == 1);
    CHECK(f.controller.contextTargetAt(ctx, Fixture::x(150), 20.0 + Fixture::bodyY(0)).clip == f.b);
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

TEST_CASE("TimelineController: with snapping off, a clip lands where it's dropped")
{
    Fixture f;
    f.snapping = false;
    TimelineOutcome out = f.drag(Fixture::x(350), Fixture::bodyY(0), Fixture::x(255), Fixture::bodyY(0));
    CHECK(f.apply(out) == 0);
    CHECK(f.model.clip(f.c).position == 205);
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

TEST_CASE("TimelineController: Shift adds to the selection, Ctrl toggles, neither seeks")
{
    Fixture f;
    f.drag(Fixture::x(50), Fixture::bodyY(0), Fixture::x(50), Fixture::bodyY(0));
    TimelineOutcome shift =
        f.drag(Fixture::x(350), Fixture::bodyY(0), Fixture::x(350), Fixture::bodyY(0), Modifiers::Shift);
    CHECK_FALSE(shift.seek);
    CHECK(f.controller.selection().clips() == std::set<ClipId>{f.a, f.c});

    f.drag(Fixture::x(50), Fixture::bodyY(0), Fixture::x(50), Fixture::bodyY(0), Modifiers::Ctrl);
    CHECK(f.controller.selection().clips() == std::set<ClipId>{f.c});
    f.drag(Fixture::x(550), Fixture::bodyY(1), Fixture::x(550), Fixture::bodyY(1), Modifiers::Ctrl);
    CHECK(f.controller.selection().clips() == std::set<ClipId>{f.c, f.d});

    // A plain click on one of them narrows the selection to it.
    f.drag(Fixture::x(350), Fixture::bodyY(0), Fixture::x(350), Fixture::bodyY(0));
    CHECK(f.controller.selection().clips() == std::set<ClipId>{f.c});
}

TEST_CASE("TimelineController: dragging one of several selected clips moves them all")
{
    Fixture f;
    f.controller.selection().add(f.c);
    f.controller.selection().add(f.d);
    TimelineOutcome out = f.drag(Fixture::x(350), Fixture::bodyY(0), Fixture::x(400), Fixture::bodyY(0));
    CHECK(f.apply(out) == 0);
    CHECK(f.model.clip(f.c).position == 350);
    CHECK(f.model.clip(f.d).position == 550);
    CHECK(f.model.clip(f.a).position == 0);
    CHECK(f.undo.undo());
    CHECK(f.model.clip(f.c).position == 300);
    CHECK(f.model.clip(f.d).position == 500);
}

TEST_CASE("TimelineController: a group can't be dragged off the track list or before frame 0")
{
    Fixture f;
    f.controller.selection().add(f.c);
    f.controller.selection().add(f.d);
    f.controller.press(f.ctx(), Fixture::x(350), Fixture::bodyY(0), Modifiers::None);
    f.controller.motion(f.ctx(), -900.0, 300.0); // far left and far down
    CHECK(f.controller.preview().group);
    CHECK(f.controller.preview().groupRowDelta == 0); // d is already on the last track
    CHECK(f.controller.preview().groupDelta == -300); // c stops at frame 0
}

TEST_CASE("TimelineController: Shift+drag on empty space selects what the box touches")
{
    Fixture f;
    // From the gap on V1 (frame 250) down-right into V2 past d's start.
    TimelineOutcome out =
        f.drag(Fixture::x(250), Fixture::bodyY(0), Fixture::x(520), Fixture::bodyY(1), Modifiers::Shift);
    CHECK(out.attempts.empty());
    CHECK_FALSE(out.seek);
    CHECK(f.controller.selection().clips() == std::set<ClipId>{f.c, f.d});
}

TEST_CASE("TimelineController: select all")
{
    Fixture f;
    f.controller.selectAll(f.model);
    CHECK(f.controller.selection().clips().size() == 4);
}

TEST_CASE("TimelineController: Alt+drag on a tail ripple trims; later clips follow")
{
    Fixture f;
    TimelineOutcome out = f.drag(Fixture::x(99), Fixture::bodyY(0), Fixture::x(109), Fixture::bodyY(0), Modifiers::Alt);
    CHECK(f.apply(out) == 0);
    CHECK(f.model.clip(f.a).end() == 110);
    CHECK(f.model.clip(f.b).position == 110);
    CHECK(f.model.clip(f.c).position == 310);
    CHECK(f.model.clip(f.d).position == 500); // other tracks don't move
}

TEST_CASE("TimelineController: Alt+drag on a head cuts the front; the clip stays put, later ones close up")
{
    Fixture f;
    f.controller.press(f.ctx(), Fixture::x(101), Fixture::bodyY(0), Modifiers::Alt);
    CHECK(f.controller.mode() == TimelineController::Mode::RippleTrimStart);
    f.controller.motion(f.ctx(), 10.0, 0.0);
    CHECK(f.controller.preview().start == 100);
    CHECK(f.controller.preview().length == 90);
    TimelineOutcome out = f.controller.release(f.ctx(), 10.0, 0.0);
    CHECK(f.apply(out) == 0);
    CHECK(f.model.clip(f.b).position == 100);
    CHECK(f.model.clip(f.b).length() == 90);
    CHECK(f.model.clip(f.b).in == 2010);
    CHECK(f.model.clip(f.c).position == 290);
}

TEST_CASE("TimelineController: Shift+drag on an edge slips the source, not the clip")
{
    Fixture f;
    TimelineOutcome out =
        f.drag(Fixture::x(399), Fixture::bodyY(0), Fixture::x(414), Fixture::bodyY(0), Modifiers::Shift);
    CHECK(f.apply(out) == 0);
    CHECK(f.model.clip(f.c).position == 300);
    CHECK(f.model.clip(f.c).length() == 100);
    CHECK(f.model.clip(f.c).in == 2985); // dragged right: earlier frames
}

TEST_CASE("TimelineController: Ctrl+drag copies; Ctrl+click still only toggles")
{
    Fixture f;
    TimelineOutcome out =
        f.drag(Fixture::x(550), Fixture::bodyY(1), Fixture::x(750), Fixture::bodyY(0), Modifiers::Ctrl);
    CHECK(f.apply(out) == 0);
    CHECK(f.model.clip(f.d).position == 500); // the original stays
    const Track &v1 = f.model.track(f.v1);
    REQUIRE(v1.clips.size() == 4);
    const Clip &copy = f.model.clip(v1.clips.back());
    CHECK(copy.position == 700);
    CHECK(copy.in == 4000);
    CHECK(f.controller.selection().empty()); // a copy doesn't change the selection

    f.drag(Fixture::x(550), Fixture::bodyY(1), Fixture::x(550), Fixture::bodyY(1), Modifiers::Ctrl);
    CHECK(f.controller.selection().clips() == std::set<ClipId>{f.d});
}

TEST_CASE("TimelineController: a slip stops at the start of the source")
{
    Fixture f;
    // c's source starts at 3000; asking for 5000 earlier frames stops at 0.
    f.controller.press(f.ctx(), Fixture::x(399), Fixture::bodyY(0), Modifiers::Shift);
    f.controller.motion(f.ctx(), 5000.0, 0.0);
    CHECK(f.controller.preview().slipDelta == -3000);
}

TEST_CASE("TimelineController: a move or copy onto something shows as invalid while dragging")
{
    Fixture f;
    f.controller.press(f.ctx(), Fixture::x(350), Fixture::bodyY(0), Modifiers::None);
    f.controller.motion(f.ctx(), 100.0, 0.0); // c into the free space after it
    CHECK(f.controller.preview().valid);
    f.controller.motion(f.ctx(), -180.0, 0.0); // c onto b
    CHECK_FALSE(f.controller.preview().valid);
    f.controller.cancel();

    f.controller.press(f.ctx(), Fixture::x(550), Fixture::bodyY(1), Modifiers::Ctrl);
    f.controller.motion(f.ctx(), -500.0, -60.0); // a copy of d onto a
    CHECK_FALSE(f.controller.preview().valid);
    f.controller.motion(f.ctx(), 200.0, -60.0); // onto free V1 space at 700
    CHECK(f.controller.preview().valid);
}

TEST_CASE("TimelineController: in ripple mode a move closes its gap and pushes later clips")
{
    Fixture f;
    TimelineContext ctx = f.ctx();
    ctx.rippleMode = true;
    // a [0,100) dropped at d's place on V2 (frame 500): d moves right by 100;
    // V1 closes up behind a.
    f.controller.press(ctx, Fixture::x(50), Fixture::bodyY(0), Modifiers::None);
    f.controller.motion(ctx, 500.0, 60.0);
    CHECK(f.controller.preview().valid); // overlapping d is fine: room is made
    TimelineOutcome out = f.controller.release(ctx, 500.0, 60.0);
    CHECK(f.apply(out) == 0);
    CHECK(f.model.clip(f.a).track == f.v2);
    CHECK(f.model.clip(f.a).position == 500);
    CHECK(f.model.clip(f.d).position == 600);
    CHECK(f.model.clip(f.b).position == 0);
    CHECK(f.model.clip(f.c).position == 200);
}

TEST_CASE("TimelineController: in ripple mode a drop inside a clip goes to its nearer edge")
{
    Fixture f;
    TimelineContext ctx = f.ctx();
    ctx.rippleMode = true;
    // d [500,600) dropped with its start at 330 on V1, inside c [300,400):
    // nearer c's start, so it lands at 300 and c moves to 400.
    f.controller.press(ctx, Fixture::x(550), Fixture::bodyY(1), Modifiers::None);
    f.controller.motion(ctx, -170.0, -60.0);
    CHECK(f.controller.preview().start == 300);
    TimelineOutcome out = f.controller.release(ctx, -170.0, -60.0);
    CHECK(f.apply(out) == 0);
    CHECK(f.model.clip(f.d).position == 300);
    CHECK(f.model.clip(f.c).position == 400);
}

TEST_CASE("TimelineController: a short ripple drag inside the clip's own span is no move (post-M3 audit P6)")
{
    Fixture f;
    TimelineContext ctx = f.ctx();
    ctx.rippleMode = true;
    // a [0,100) nudged 50 frames right, still inside its own span; b butts it.
    f.controller.press(ctx, Fixture::x(20), Fixture::bodyY(0), Modifiers::None);
    f.controller.motion(ctx, 50.0, 0.0);
    CHECK(f.controller.preview().start == 0);
    CHECK(f.controller.preview().valid);
    TimelineOutcome out = f.controller.release(ctx, 50.0, 0.0);
    CHECK(out.attempts.empty());
    CHECK(f.model.clip(f.a).position == 0);
}
