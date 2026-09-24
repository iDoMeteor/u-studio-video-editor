// doc 19 MT2 piece 2: the engine thread, through its main-thread facade.
// PlaybackController's own behaviours (test_playback_controller.cpp) held
// again here through engine::Engine -- pause shows the exact frame, quick
// steps each count, loops wrap, an edit while paused keeps the frame,
// shutdown during playback is clean -- plus what the thread adds: snapshots
// are latest-wins and the main thread never waits on a build.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/model/model.h"
#include "engine/engine.h"
#include "engine/factory_policy.h"

#include <glib.h>

#include <chrono>
#include <thread>
#include <vector>

using namespace ustudio::core;
using namespace ustudio::engine;

namespace {

FactoryPolicy &sharedFactoryPolicy()
{
    static FactoryPolicy policy;
    return policy;
}

// One video track of `clips` ten-frame colour clips.
Model makeModel(int clips)
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset asset;
    asset.path = "color:red";
    asset.displayName = "red";
    asset.info.hasVideo = true;
    asset.info.lengthInSequenceFrames = 100'000;
    AssetId id = model.addAsset(asset);
    for (int c = 0; c < clips; ++c)
        model.insertClip(track, id, c * 10, 0, 9);
    return model;
}

template <class Done> bool pumpUntil(Done done, std::chrono::milliseconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_iteration(nullptr, FALSE)) {
        }
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

struct Frames
{
    std::vector<int> positions;
    void attach(Engine &engine)
    {
        engine.setFrameCallback(
            [this](std::vector<uint8_t>, int, int, int position) { positions.push_back(position); });
    }
};

} // namespace

TEST_CASE("Engine: starts on its own thread and mirrors the graph's length and rate")
{
    sharedFactoryPolicy();
    Model model = makeModel(5);
    Engine engine(model.snapshot(), PreviewScale::Full);
    REQUIRE(engine.syncForTesting());
    CHECK(engine.totalFrames() == 50);
    CHECK(engine.fps() > 0.0);
    CHECK_FALSE(engine.backendName().empty());
    engine.shutdown();
}

TEST_CASE("Engine: pause shows the exact frame sought to, not one off")
{
    sharedFactoryPolicy();
    Model model = makeModel(5);
    Engine engine(model.snapshot(), PreviewScale::Full);
    Frames frames;
    frames.attach(engine);
    REQUIRE(engine.syncForTesting());
    pumpUntil([&] { return !frames.positions.empty(); }, std::chrono::seconds(2));

    frames.positions.clear();
    engine.seek(30);
    CHECK(engine.currentFrame() == 30); // mirrored at call time
    REQUIRE(
        pumpUntil([&] { return !frames.positions.empty() && frames.positions.back() == 30; }, std::chrono::seconds(3)));
    REQUIRE(engine.syncForTesting());
    CHECK(engine.currentFrame() == 30);
    CHECK(frames.positions.back() == 30);
}

TEST_CASE("Engine: back-to-back frame steps while paused each count, before and after the engine catches up")
{
    sharedFactoryPolicy();
    Model model = makeModel(5);
    Engine engine(model.snapshot(), PreviewScale::Full);
    REQUIRE(engine.syncForTesting());

    // No pumping between steps: each must advance from the previous step's
    // target, not from a frame or state report that hasn't arrived yet.
    engine.stepFrame(1);
    engine.stepFrame(1);
    engine.stepFrame(1);
    CHECK(engine.currentFrame() == 3);
    engine.stepFrame(-1);
    CHECK(engine.currentFrame() == 2);
    REQUIRE(engine.syncForTesting()); // the engine's own reports agree
    CHECK(engine.currentFrame() == 2);
}

TEST_CASE("Engine: a loop range wraps playback back to loop-in at loop-out")
{
    sharedFactoryPolicy();
    Model model = makeModel(6);
    Engine engine(model.snapshot(), PreviewScale::Full);
    Frames frames;
    frames.attach(engine);
    REQUIRE(engine.syncForTesting());
    engine.setLoopRange(std::make_pair(5, 15));
    engine.seek(5);
    engine.play(1.0);
    bool wrapped = pumpUntil(
        [&] {
            for (size_t i = 1; i < frames.positions.size(); ++i)
                if (frames.positions[i] < frames.positions[i - 1])
                    return true;
            return false;
        },
        std::chrono::seconds(10));
    engine.pause();
    REQUIRE(wrapped);
    for (int position : frames.positions)
        CHECK(position <= 20); // loop-out + 5, as the controller's own test allows
}

TEST_CASE("Engine: an edit while paused keeps showing the paused frame")
{
    sharedFactoryPolicy();
    Model model = makeModel(5);
    Engine engine(model.snapshot(), PreviewScale::Full);
    Frames frames;
    frames.attach(engine);
    int rebuilds = 0;
    engine.rebuilt.connect([&] { ++rebuilds; });
    REQUIRE(engine.syncForTesting());
    engine.seek(23);
    REQUIRE(engine.syncForTesting());
    rebuilds = 0;

    model.insertClip(model.sequence().tracks.front().id, model.project().bin.front().id, 100, 0, 9);
    engine.publish(model.snapshot());
    REQUIRE(engine.syncForTesting());
    CHECK(rebuilds == 1);
    CHECK(engine.totalFrames() == 110);
    CHECK(engine.currentFrame() == 23);
    frames.positions.clear();
    REQUIRE(pumpUntil([&] { return !frames.positions.empty(); }, std::chrono::seconds(3)));
    CHECK(frames.positions.back() == 23);
}

TEST_CASE("Engine: a seek sent after an edit lands on the new, longer graph")
{
    sharedFactoryPolicy();
    Model model = makeModel(2); // 20 frames
    Engine engine(model.snapshot(), PreviewScale::Full);
    REQUIRE(engine.syncForTesting());
    model.insertClip(model.sequence().tracks.front().id, model.project().bin.front().id, 90, 0, 9);
    engine.publish(model.snapshot());
    engine.seek(95); // the mirror still says 20 frames; the engine must not clamp to it
    REQUIRE(engine.syncForTesting());
    CHECK(engine.totalFrames() == 100);
    CHECK(engine.currentFrame() == 95);
}

TEST_CASE("Engine: a burst of snapshots is latest-wins, and the main thread never waits on a build")
{
    sharedFactoryPolicy();
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    constexpr bool kSanitized = true;
    Model model = makeModel(300); // builds are several times slower here
#else
    constexpr bool kSanitized = false;
    Model model = makeModel(2000); // a build long enough to queue behind
#endif
    Engine engine(model.snapshot(), PreviewScale::Full);
    int rebuilds = 0;
    engine.rebuilt.connect([&] { ++rebuilds; });
    REQUIRE(engine.syncForTesting());
    rebuilds = 0;

    TrackId track = model.sequence().tracks.front().id;
    AssetId asset = model.project().bin.front().id;
    double slowestCallMs = 0;
    for (int i = 0; i < 20; ++i) {
        model.insertClip(track, asset, 30'000 + i * 10, 0, 9);
        auto start = std::chrono::steady_clock::now();
        engine.publish(model.snapshot());
        engine.seek(i);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        slowestCallMs = std::max(slowestCallMs, ms);
    }
    REQUIRE(engine.syncForTesting());
    CAPTURE(rebuilds);
    CAPTURE(slowestCallMs);
    // Each publish is followed by a seek, so snapshots don't sit next to
    // each other in the queue: the burst collapses only where the engine
    // fell behind. It must never take more builds than edits.
    CHECK(rebuilds >= 1);
    CHECK(rebuilds <= 20);
    CHECK(engine.totalFrames() == 30'000 + 19 * 10 + 10);
    if (kSanitized)
        MESSAGE("sanitizer build: slowest publish+seek " << slowestCallMs << " ms, reported, not checked");
    else
        CHECK(slowestCallMs < 16.0);

    // Back to back with nothing between, twenty publishes collapse to at
    // most one build beyond the one already running.
    // Snapshots made first: copying a 2,000-clip project for each one
    // inside the loop spreads the burst over more than one build's time.
    std::vector<std::shared_ptr<const Project>> burst;
    for (int i = 0; i < 20; ++i) {
        model.insertClip(track, asset, 40'000 + i * 10, 0, 9);
        burst.push_back(model.snapshot());
    }
    rebuilds = 0;
    for (const auto &snapshot : burst)
        engine.publish(snapshot);
    REQUIRE(engine.syncForTesting());
    CAPTURE(rebuilds);
    CHECK(rebuilds <= 2);
    CHECK(engine.totalFrames() == 40'000 + 19 * 10 + 10);
}

TEST_CASE("Engine: shutdown during playback is clean, repeated")
{
    sharedFactoryPolicy();
    Model model = makeModel(5);
    for (int i = 0; i < 30; ++i) {
        Engine engine(model.snapshot(), PreviewScale::Full);
        engine.play(1.0);
        if (i % 3 == 0)
            REQUIRE(engine.syncForTesting());
        engine.shutdown();
        CHECK(engine.totalFrames() >= 0);
    }
}
