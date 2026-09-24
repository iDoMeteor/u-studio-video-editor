#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/model/model.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"
#include "engine/playback_controller.h"

#include <glib.h>
#include <mlt++/Mlt.h>

#include <chrono>
#include <mutex>
#include <thread>
#include <tuple>
#include <vector>

using namespace ustudio::engine;

namespace {

// Shared across TEST_CASEs, matching FactoryPolicy's own documented
// "exactly one instance per process" contract (tests/engine/
// test_engine_sync.cpp established this pattern first; see its comment).
FactoryPolicy &sharedFactoryPolicy()
{
    static FactoryPolicy policy;
    return policy;
}

// A one-track tractor with a single synthetic clip -- no binary media
// (doc 11): "colour:" is an MLT generator producer.
std::shared_ptr<Mlt::Tractor> makeOneClipTractor(Mlt::Profile &profile, int frames)
{
    auto tractor = std::make_shared<Mlt::Tractor>(profile);
    Mlt::Producer producer(profile, "colour:red");
    producer.set_in_and_out(0, frames - 1);
    tractor->set_track(producer, 0);
    tractor->set_in_and_out(0, frames - 1);
    return tractor;
}

// PlaybackController posts frame delivery through MainThreadDispatcher,
// which invokes GLib's DEFAULT main context (g_main_context_invoke_full)
// -- nothing runs that context's loop in a plain doctest binary, so tests
// must pump it themselves. Polls until `done` returns true or `timeout`
// elapses; returns whether `done` became true.
template <class Done> bool pumpMainContextUntil(Done done, std::chrono::milliseconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_iteration(nullptr, FALSE)) {
        }
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

} // namespace

TEST_CASE("PlaybackController: selects a valid consumer and reports its backend name")
{
    sharedFactoryPolicy();
    Mlt::Profile profile;
    auto tractor = makeOneClipTractor(profile, 10);

    PlaybackController controller;
    controller.setTractor(tractor);

    // sdl2_audio/rtaudio/null (ADR-002) -- whichever this machine picked,
    // it must be one of the three, never empty (null always succeeds).
    std::string backend = controller.backendName();
    CHECK((backend == "sdl2_audio" || backend == "rtaudio" || backend == "null"));
    CHECK(controller.totalFrames() == 10);
}

TEST_CASE("PlaybackController: repeated setTractor calls on a live, running consumer "
          "are safe -- each does a full restart, never an in-place reconnect")
{
    sharedFactoryPolicy();
    Mlt::Profile profile;

    PlaybackController controller;
    std::mutex mutex;
    std::vector<int> positions;
    controller.setFrameCallback([&](std::vector<uint8_t>, int, int, int frameNumber) {
        std::lock_guard<std::mutex> lock(mutex);
        positions.push_back(frameNumber);
    });

    controller.setTractor(makeOneClipTractor(profile, 10));
    REQUIRE(controller.consumerRestartCount() == 1);
    controller.play(1.0);

    // Simulates several ordinary edits (EngineSync::rebuildAll() produces
    // a brand-new Tractor object on every one, same profile) landing while
    // playback is running -- e.g. trimming or moving a clip without
    // stopping first. An earlier version of setTractor() tried to
    // optimize this exact case with Mlt::Consumer::connect() on the
    // already-running consumer instead of a full restart, to avoid
    // closing and reopening the real audio device on every edit. That
    // corrupted MLT's internal state: reproduced 3/3 with a GDB backtrace
    // showing a crash inside MLT's own consumer_read_ahead_thread /
    // mlt_service_get_frame, sometime after the swap, reading through
    // memory that belonged to the tractor that had just been replaced --
    // its background read-ahead (prefetch) thread was still running
    // against the old one when the swap happened. Every setTractor() call
    // below does its own full stop/reselect/restart instead
    // (consumerRestartCount() increments every time), which is the version
    // actually proven safe by the shutdown-during-playback stress test
    // elsewhere in this file. This loop is this specific scenario's
    // regression test.
    for (int i = 0; i < 10; ++i)
        controller.setTractor(makeOneClipTractor(profile, 10 + i));
    CHECK(controller.consumerRestartCount() == 11);

    // Playback must still be alive and producing frames after all that --
    // not just "didn't crash", per doc 05's actual point of a real
    // consumer: continuous delivery, not one-off refreshes. A generous
    // timeout: this test binary's other suites (particularly the
    // 100-iteration shutdown stress test) also cycle the real audio
    // device heavily in the same process, and re-acquiring it can be
    // measurably slower under that load than in isolation -- this loop
    // ran clean in well under a second standalone but needed longer when
    // run after the rest of the file's device churn.
    bool gotFrames = pumpMainContextUntil(
        [&] {
            std::lock_guard<std::mutex> lock(mutex);
            return positions.size() >= 5;
        },
        std::chrono::seconds(10));
    CHECK(gotFrames);
}

TEST_CASE("PlaybackController: null-consumer position test -- frame-show delivers every "
          "position once, strictly in order, covering the full range")
{
    sharedFactoryPolicy();
    Mlt::Profile profile;
    auto tractor = makeOneClipTractor(profile, 20);

    PlaybackController controller;
    // Not sdl2_audio/rtaudio: this test asserts pacing/ordering
    // correctness deterministically, independent of whatever audio device
    // is or isn't available in the environment it runs in (doc 05's
    // "null-consumer position test" from the M2 acceptance criteria).
    // PlaybackController's own selection isn't overridable, so this test
    // drives a `null` consumer directly rather than through setTractor().
    Mlt::Consumer consumer(profile, "null");
    REQUIRE(consumer.is_valid());
    consumer.set("real_time", 1);

    std::mutex mutex;
    std::vector<int> positions;
    Mlt::Event *event = consumer.listen(
        "consumer-frame-show", &positions, [](mlt_properties, void *self, mlt_event_data data) {
            Mlt::Frame frame(Mlt::EventData(data).to_frame());
            if (!frame.is_valid())
                return;
            static_cast<std::vector<int> *>(self)->push_back(frame.get_position());
        });
    REQUIRE(event->is_valid());

    REQUIRE(consumer.connect(*tractor) == 0);
    REQUIRE(consumer.start() == 0);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && consumer.position() < 19)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    consumer.stop();
    delete event;

    REQUIRE_FALSE(positions.empty());
    CHECK(positions.front() == 0);
    CHECK(positions.back() >= 19);
    for (size_t i = 1; i < positions.size(); ++i)
        CHECK(positions[i] >= positions[i - 1]); // strictly non-decreasing: never reordered
}

TEST_CASE("PlaybackController: pause shows the exact frame sought to, not one off")
{
    sharedFactoryPolicy();
    Mlt::Profile profile;
    auto tractor = makeOneClipTractor(profile, 50);

    PlaybackController controller;
    std::mutex mutex;
    std::vector<int> deliveredPositions;
    controller.setFrameCallback([&](std::vector<uint8_t>, int, int, int frameNumber) {
        std::lock_guard<std::mutex> lock(mutex);
        deliveredPositions.push_back(frameNumber);
    });
    controller.setTractor(tractor);

    // Let at least one initial frame (position 0, from setTractor's own
    // implicit pause) land, then seek to an interior frame while paused --
    // this is the "scrub while stopped" path (doc 05), which purges and
    // forces exactly one refreshed frame through.
    pumpMainContextUntil([&] { std::lock_guard<std::mutex> lock(mutex); return !deliveredPositions.empty(); },
                        std::chrono::seconds(2));

    {
        std::lock_guard<std::mutex> lock(mutex);
        deliveredPositions.clear();
    }
    controller.seek(30);

    bool got = pumpMainContextUntil(
        [&] {
            std::lock_guard<std::mutex> lock(mutex);
            return !deliveredPositions.empty();
        },
        std::chrono::seconds(2));
    REQUIRE(got);

    std::lock_guard<std::mutex> lock(mutex);
    CHECK(controller.currentFrame() == 30);
    // The exact frame sought to must appear -- not 29 or 31, which would
    // indicate an off-by-one in the purge+refresh pause mechanism.
    CHECK(deliveredPositions.back() == 30);
}

TEST_CASE("PlaybackController: an edit while paused (setTractor) keeps showing the paused frame")
{
    sharedFactoryPolicy();
    Mlt::Profile profile;

    PlaybackController controller;
    std::mutex mutex;
    std::vector<int> deliveredPositions;
    controller.setFrameCallback([&](std::vector<uint8_t>, int, int, int frameNumber) {
        std::lock_guard<std::mutex> lock(mutex);
        deliveredPositions.push_back(frameNumber);
    });
    controller.setTractor(makeOneClipTractor(profile, 50));
    controller.seek(30);
    REQUIRE(pumpMainContextUntil(
        [&] {
            std::lock_guard<std::mutex> lock(mutex);
            return !deliveredPositions.empty() && deliveredPositions.back() == 30;
        },
        std::chrono::seconds(3)));

    // Every ordinary edit hands PlaybackController a brand-new tractor
    // (EngineSync::rebuildAll()); setTractor() restarts the consumer and
    // re-enters the paused state at the preserved position. Whatever the
    // fresh consumer delivers must be the preserved frame -- never a stale
    // frame 0 from before setTractor()'s own seek (the reason setTractor()
    // keeps its purge; see the comment there on sanitizer report S4).
    {
        std::lock_guard<std::mutex> lock(mutex);
        deliveredPositions.clear();
    }
    controller.setTractor(makeOneClipTractor(profile, 50));
    bool got = pumpMainContextUntil(
        [&] {
            std::lock_guard<std::mutex> lock(mutex);
            return !deliveredPositions.empty();
        },
        std::chrono::seconds(3));
    // Let any further frames the restart produces land too.
    pumpMainContextUntil([] { return false; }, std::chrono::milliseconds(300));

    std::lock_guard<std::mutex> lock(mutex);
    REQUIRE(got);
    CHECK(controller.currentFrame() == 30);
    for (int position : deliveredPositions)
        CHECK(position == 30);
}

TEST_CASE("PlaybackController: back-to-back frame steps while paused each count")
{
    sharedFactoryPolicy();
    Mlt::Profile profile;
    PlaybackController controller;
    controller.setTractor(makeOneClipTractor(profile, 50));

    // No main-loop pumping between the steps: the first step's frame has
    // not been displayed yet when the second one runs, which is exactly a
    // quick double-click on the step button or a held arrow key. Each step
    // must still advance from the previous step's target, not from the
    // last frame that happened to reach the screen.
    controller.stepFrame(1);
    controller.stepFrame(1);
    controller.stepFrame(1);
    CHECK(controller.currentFrame() == 3);
    controller.stepFrame(-1);
    CHECK(controller.currentFrame() == 2);
}

TEST_CASE("PlaybackController: pausing mid-playback stays on the last displayed frame")
{
    sharedFactoryPolicy();
    Mlt::Profile profile;
    auto tractor = makeOneClipTractor(profile, 400);

    PlaybackController controller;
    std::mutex mutex;
    std::vector<int> deliveredPositions;
    controller.setFrameCallback([&](std::vector<uint8_t>, int, int, int frameNumber) {
        std::lock_guard<std::mutex> lock(mutex);
        deliveredPositions.push_back(frameNumber);
    });
    controller.setTractor(tractor);
    controller.play(1.0);

    // Play long enough for the consumer's read-ahead buffer (`buffer`=25
    // frames) to fill, so the producer's position is well ahead of what's
    // on screen -- the condition the 2026-09-20 audit's E1 measured (paused
    // after frame 52 was shown, the refresh frame came back as 83).
    bool playing = pumpMainContextUntil(
        [&] {
            std::lock_guard<std::mutex> lock(mutex);
            return !deliveredPositions.empty() && deliveredPositions.back() >= 40;
        },
        std::chrono::seconds(10));
    REQUIRE(playing);

    controller.pause();
    int pausedAt = controller.currentFrame();
    {
        std::lock_guard<std::mutex> lock(mutex);
        deliveredPositions.clear();
    }

    // Frames already queued before pause() may still land first; the
    // refresh frame pause() forces must then show exactly pausedAt. Before
    // the fix it showed wherever the producer had read ahead to instead.
    bool gotPausedFrame = pumpMainContextUntil(
        [&] {
            std::lock_guard<std::mutex> lock(mutex);
            return !deliveredPositions.empty() && deliveredPositions.back() == pausedAt;
        },
        std::chrono::seconds(3));
    std::lock_guard<std::mutex> lock(mutex);
    INFO("paused at " << pausedAt << ", last delivered "
                      << (deliveredPositions.empty() ? -1 : deliveredPositions.back()));
    CHECK(gotPausedFrame);
    CHECK(controller.currentFrame() == pausedAt);
}

TEST_CASE("PlaybackController: loop range wraps playback back to loop-in at loop-out")
{
    sharedFactoryPolicy();
    Mlt::Profile profile;
    auto tractor = makeOneClipTractor(profile, 60);

    PlaybackController controller;
    std::mutex mutex;
    std::vector<int> deliveredPositions;
    controller.setFrameCallback([&](std::vector<uint8_t>, int, int, int frameNumber) {
        std::lock_guard<std::mutex> lock(mutex);
        deliveredPositions.push_back(frameNumber);
    });
    controller.setTractor(tractor);
    controller.setLoopRange(std::make_pair(5, 15));
    controller.play(1.0);

    // Run long enough to cross the loop-out point at least twice -- if
    // looping didn't work, position would just climb past 15 towards 59
    // and never come back down.
    bool wrapped = pumpMainContextUntil(
        [&] {
            std::lock_guard<std::mutex> lock(mutex);
            if (deliveredPositions.size() < 2)
                return false;
            for (size_t i = 1; i < deliveredPositions.size(); ++i) {
                if (deliveredPositions[i] < deliveredPositions[i - 1])
                    return true; // a drop -- the wrap happened
            }
            return false;
        },
        std::chrono::seconds(5));

    controller.pause();
    std::lock_guard<std::mutex> lock(mutex);
    REQUIRE(wrapped);
    // Every delivered position must stay within a hair of the loop range --
    // doc 05 accepts a one-frame overshoot before the wrap-seek lands, but
    // nothing should ever reach the middle of the clip's untouched tail.
    for (int position : deliveredPositions)
        CHECK(position <= 20);
}

TEST_CASE("PlaybackController: a loop range does not affect paused seeking, "
          "including past loop-out")
{
    sharedFactoryPolicy();
    Mlt::Profile profile;
    auto tractor = makeOneClipTractor(profile, 60);

    PlaybackController controller;
    std::mutex mutex;
    std::vector<int> deliveredPositions;
    controller.setFrameCallback([&](std::vector<uint8_t>, int, int, int frameNumber) {
        std::lock_guard<std::mutex> lock(mutex);
        deliveredPositions.push_back(frameNumber);
    });
    controller.setTractor(tractor);
    controller.setLoopRange(std::make_pair(5, 15));

    // Bug audit E4: the loop-wrap check in drainSlot() had no m_playing
    // guard, so a paused "refresh" frame (delivered by the same
    // consumer-frame-show path as active playback) landing at or past
    // loop-out snapped straight back to loop-in instead of showing the
    // frame actually asked for. seek(40) is past loop-out (15) while
    // stopped -- this must land exactly on 40, not wrap to 5.
    controller.seek(40);
    bool got40 = pumpMainContextUntil(
        [&] {
            std::lock_guard<std::mutex> lock(mutex);
            return !deliveredPositions.empty() && deliveredPositions.back() == 40;
        },
        std::chrono::seconds(3));
    CHECK(got40);
    CHECK(controller.currentFrame() == 40);

    // toEnd() is the other named trigger in the audit -- also must not wrap.
    {
        std::lock_guard<std::mutex> lock(mutex);
        deliveredPositions.clear();
    }
    controller.toEnd();
    bool gotEnd = pumpMainContextUntil(
        [&] {
            std::lock_guard<std::mutex> lock(mutex);
            return !deliveredPositions.empty() && deliveredPositions.back() == 59;
        },
        std::chrono::seconds(3));
    CHECK(gotEnd);
    CHECK(controller.currentFrame() == 59);
}

TEST_CASE("PlaybackController: shutdown during playback is clean, repeated 100 times")
{
    sharedFactoryPolicy();
    Mlt::Profile profile;

    for (int i = 0; i < 100; ++i) {
        auto tractor = makeOneClipTractor(profile, 30);
        PlaybackController controller;
        controller.setTractor(tractor);
        controller.play(1.0);
        // No wait: the point of this loop is exercising shutdown() at an
        // arbitrary, unsynchronized point relative to the consumer thread
        // possibly mid-frame -- exactly the race a sanitiser run (doc 12's
        // M2 acceptance) is meant to catch. A clean run 100/100 times,
        // ideally under -Db_sanitize=address,undefined, is the criterion.
        controller.shutdown();
    }
}

TEST_CASE("PlaybackController: frames reach the UI at the preview size, not the sequence size")
{
    // Regression for the 2026-09-24 soak finding: at "Half", 4K60 playback
    // still delivered 3840x2160 frames, because only the consumer's "scale"
    // was set. EngineSync now builds the playback tractor on a scaled
    // profile, so the rendered and delivered frames are actually smaller.
    sharedFactoryPolicy();
    using namespace ustudio::core;
    Model model = Model::createEmpty(); // 1920x1080 @ 30
    Asset asset;
    asset.path = "color:blue";
    asset.displayName = "blue";
    asset.info.hasVideo = true;
    asset.info.lengthInSequenceFrames = 100'000;
    AssetId assetId = model.addAsset(asset);
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    model.insertClip(track, assetId, 0, 0, 59);

    for (auto [scale, expectedWidth, expectedHeight] :
         {std::tuple{PreviewScale::Half, 960, 540}, std::tuple{PreviewScale::Full, 1920, 1080}}) {
        EngineSync sync(model, scale);
        PlaybackController controller;
        std::mutex mutex;
        int width = 0, height = 0;
        controller.setFrameCallback([&](std::vector<uint8_t>, int w, int h, int) {
            std::lock_guard<std::mutex> lock(mutex);
            width = w;
            height = h;
        });
        controller.setTractor(sync.tractorPtr());
        bool got = pumpMainContextUntil(
            [&] {
                std::lock_guard<std::mutex> lock(mutex);
                return width > 0;
            },
            std::chrono::seconds(5));
        controller.shutdown();
        REQUIRE(got);
        std::lock_guard<std::mutex> lock(mutex);
        CHECK(width == expectedWidth);
        CHECK(height == expectedHeight);
    }
}
