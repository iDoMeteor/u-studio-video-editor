#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/timeline/timeline_controller.h"
#include "core/commands/undo_stack.h"
#include "core/model/model.h"

#include <random>

using namespace ustudio::core;
using namespace ustudio::app::timeline;

namespace {

bool equalIgnoringIdAllocator(const Model &a, const Model &b)
{
    Project pa = a.project();
    Project pb = b.project();
    pa.nextId = pb.nextId = 0;
    return pa == pb;
}

} // namespace

// doc 12, M3: "no model invariant violation is reachable via UI (fuzz ...
// random drags ... under the debug verifier)". The same gestures the
// window forwards, at random places with random modifiers, straight into
// the controller and the undo stack; Model::check() after every one, and
// undoing everything must give back the starting project.
void fuzz(unsigned seed)
{
    Model model = Model::createEmpty();
    UndoStack undo(model);
    undo.limit = 1'000'000;
    Asset video;
    video.path = "color:red";
    video.info.hasVideo = true;
    video.info.hasAudio = true;
    video.info.lengthInSequenceFrames = 100'000;
    AssetId asset = model.addAsset(video);
    std::vector<TrackId> tracks = {model.addTrack(Track::Kind::Video, 0, "V2"),
                                   model.addTrack(Track::Kind::Video, 1, "V1"),
                                   model.addTrack(Track::Kind::Audio, 2, "A1")};
    std::mt19937 rng(seed);
    for (int i = 0; i < 12; ++i) {
        TrackId track = tracks[static_cast<size_t>(i % 3)];
        ClipId clip = model.insertClip(track, asset, (i / 3) * 150 + (i % 2) * 20, 5000 + i * 200, 5099 + i * 200);
        if (i % 3 == 2)
            model.setClipEnabled(clip, false, true);
    }
    ClipId first = model.track(tracks[1]).clips[0];
    ClipId second = model.track(tracks[1]).clips[1];
    model.moveClip(second, tracks[1], model.clip(first).end());
    model.addTransition(tracks[1], first, second, 5, 5);
    model.addMarker(300, "m");
    REQUIRE(model.check().empty());
    Model start = model;

    Viewport viewport;
    viewport.setOriginX(22.0);
    viewport.setVisibleWidth(1000.0);
    viewport.setSequenceLength(1000, 1);
    TimelineController controller;
    const Modifiers kMods[] = {Modifiers::None,  Modifiers::None, Modifiers::None,
                               Modifiers::Shift, Modifiers::Ctrl, Modifiers::Alt};

    int applied = 0;
    for (int i = 0; i < 20'000; ++i) {
        FrameIndex length = std::max<FrameIndex>(1, [&] {
            FrameIndex end = 0;
            for (const Track &t : model.sequence().tracks)
                for (ClipId id : t.clips)
                    end = std::max(end, model.clip(id).end());
            return end;
        }());
        TimelineContext ctx{.model = model,
                            .viewport = viewport,
                            .layout = RowLayout{},
                            .handleWidth = 22.0,
                            .edgeGrabPx = 8.0,
                            .dragThresholdPx = 3.0,
                            .playhead = static_cast<FrameIndex>(rng() % 1000),
                            .sequenceLength = length};
        auto rx = [&] { return static_cast<double>(rng() % 1040); };
        auto ry = [&] { return static_cast<double>(rng() % 200); };
        Modifiers mods = kMods[rng() % std::size(kMods)];

        TimelineOutcome out;
        std::string what;
        switch (rng() % 10) {
        case 0:
            out = controller.click(ctx, 2, rx(), ry(), mods);
            break;
        case 1:
            (void)controller.contextTargetAt(ctx, rx(), ry());
            continue;
        case 2:
            if (undo.canUndo())
                undo.undo();
            controller.selection().prune(model);
            continue;
        case 3:
            if (undo.canRedo())
                undo.redo();
            controller.selection().prune(model);
            continue;
        default: {
            double x = rx(), y = ry();
            controller.press(ctx, x, y, mods);
            what = "press " + std::to_string(x) + "," + std::to_string(y) + " mods " +
                   std::to_string(static_cast<unsigned>(mods)) + " mode " +
                   std::to_string(static_cast<int>(controller.mode()));
            double dx = 0, dy = 0;
            for (int m = 0, moves = static_cast<int>(rng() % 4); m < moves; ++m) {
                dx = static_cast<double>(static_cast<int>(rng() % 400) - 200);
                dy = static_cast<double>(static_cast<int>(rng() % 160) - 80);
                controller.motion(ctx, dx, dy);
            }
            out = controller.release(ctx, dx, dy);
        }
        }

        for (TimelineOutcome::Attempt &attempt : out.attempts) {
            std::string label = attempt.command->label();
            Model beforeEdit = model;
            if (undo.execute(std::move(attempt.command))) {
                if (!model.check().empty()) {
                    auto dump = [](const Model &m, const char *tag) {
                        for (const Track &t : m.sequence().tracks) {
                            std::string line = std::string(tag) + " track " + std::to_string(t.id.value) + ":";
                            for (ClipId id : t.clips) {
                                const Clip &c = m.clip(id);
                                line += " [" + std::to_string(id.value) + " " + std::to_string(c.position) + ".." +
                                        std::to_string(c.end()) + " in " + std::to_string(c.in) + "]";
                            }
                            MESSAGE(line);
                        }
                        for (const Transition &t : m.sequence().transitions)
                            MESSAGE(tag << " transition " << t.a.value << "->" << t.b.value << " eA " << t.extendA
                                        << " eB " << t.extendB);
                    };
                    dump(beforeEdit, "before");
                    dump(model, "after");
                }
                what += " -> " + label;
                ++applied;
                break;
            }
        }
        controller.selection().prune(model);
        std::vector<std::string> problems = model.check();
        if (!problems.empty()) {
            FAIL("gesture " << i << " (" << what << ") broke the model: " << problems.front());
        }
    }
    MESSAGE("seed " << seed << ": " << applied << " edits applied out of 20000 gestures");
    CHECK(applied > 500);

    while (undo.canUndo())
        REQUIRE(undo.undo());
    CHECK(equalIgnoringIdAllocator(model, start));
}

TEST_CASE("TimelineController fuzz: random gestures never break the model")
{
    for (unsigned seed : {2026u, 7u, 42u, 1234u})
        fuzz(seed);
}
