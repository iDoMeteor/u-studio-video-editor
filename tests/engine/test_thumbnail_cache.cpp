#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/model/model.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"
#include "engine/thumbnail_cache.h"
#include "engine/waveform_cache.h"
#include "sync_clip.h"

#include <glib.h>
#include <mlt++/Mlt.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <random>
#include <thread>

using namespace ustudio::engine;

namespace {

// Matches test_engine_sync.cpp's/test_waveform_cache.cpp's own documented
// "exactly one instance per process" pattern for FactoryPolicy.
FactoryPolicy &sharedFactoryPolicy()
{
    static FactoryPolicy policy;
    return policy;
}

struct RemoveOnExit
{
    std::filesystem::path path;
    ~RemoveOnExit()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

// ThumbnailCache's onReady callback is marshaled onto the default GLib
// main context (g_idle_add) -- nothing runs that context's loop in a
// plain doctest binary, so tests must pump it themselves (duplicated from
// test_waveform_cache.cpp's identical helper rather than shared across
// two small, independent test binaries).
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

// doc 19 MT3: the caches run on a pool; one for the whole binary, made after
// the shared FactoryPolicy, so it's destroyed before MLT is closed.
ustudio::core::concurrency::ThreadPool &testPool()
{
    static ustudio::core::concurrency::ThreadPool pool(4);
    return pool;
}
} // namespace

TEST_CASE("ThumbnailCache: computes a small RGBA image for a synthetic colour producer")
{
    sharedFactoryPolicy();
    std::mutex mutex;
    int readyCount = 0;
    ThumbnailCache cache(
        testPool(),
        [&] {
            std::lock_guard<std::mutex> lock(mutex);
            ++readyCount;
        },
        2, ustudio::core::concurrency::Priority::Interactive);

    // color: is an MLT generator producer -- synthetic, no file (doc 11's
    // no-binary-media rule).
    const ThumbnailCache::Data *data = cache.thumbnailFor("color:red");
    CHECK(data == nullptr); // not computed yet, first call just queues the job

    bool ready = pumpMainContextUntil(
        [&] {
            std::lock_guard<std::mutex> lock(mutex);
            return readyCount >= 1;
        },
        std::chrono::seconds(5));
    REQUIRE(ready);

    data = cache.thumbnailFor("color:red");
    REQUIRE(data != nullptr);
    CHECK(data->width > 0);
    CHECK(data->height > 0);
    CHECK(data->rgba.size() == static_cast<size_t>(data->width) * static_cast<size_t>(data->height) * 4);

    // color:red should decode as solid opaque red throughout.
    REQUIRE(data->rgba.size() >= 4);
    CHECK(data->rgba[0] > 200); // R
    CHECK(data->rgba[1] < 50);  // G
    CHECK(data->rgba[2] < 50);  // B
    CHECK(data->rgba[3] > 200); // A
}

TEST_CASE("ThumbnailCache: a second request for the same resource before it's ready doesn't queue twice")
{
    sharedFactoryPolicy();
    std::mutex mutex;
    int readyCount = 0;
    ThumbnailCache cache(
        testPool(),
        [&] {
            std::lock_guard<std::mutex> lock(mutex);
            ++readyCount;
        },
        2, ustudio::core::concurrency::Priority::Interactive);

    CHECK(cache.thumbnailFor("color:blue") == nullptr);
    CHECK(cache.thumbnailFor("color:blue") == nullptr); // still in flight -- must not double-queue

    bool ready = pumpMainContextUntil(
        [&] {
            std::lock_guard<std::mutex> lock(mutex);
            return readyCount >= 1;
        },
        std::chrono::seconds(5));
    REQUIRE(ready);

    // Give a would-be duplicate job a moment to fire a second onReady, if
    // the "already in flight" guard were broken.
    pumpMainContextUntil([] { return false; }, std::chrono::milliseconds(200));
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(readyCount == 1);
}

TEST_CASE("ThumbnailCache: an unopenable resource yields a cached, empty (not crashing) entry")
{
    sharedFactoryPolicy();
    std::mutex mutex;
    int readyCount = 0;
    ThumbnailCache cache(
        testPool(),
        [&] {
            std::lock_guard<std::mutex> lock(mutex);
            ++readyCount;
        },
        2, ustudio::core::concurrency::Priority::Interactive);

    CHECK(cache.thumbnailFor("/nonexistent/path/does-not-exist.mp4") == nullptr);
    bool ready = pumpMainContextUntil(
        [&] {
            std::lock_guard<std::mutex> lock(mutex);
            return readyCount >= 1;
        },
        std::chrono::seconds(5));
    REQUIRE(ready);

    const ThumbnailCache::Data *data = cache.thumbnailFor("/nonexistent/path/does-not-exist.mp4");
    REQUIRE(data != nullptr);
    CHECK(data->width == 0);
    CHECK(data->height == 0);
    CHECK(data->rgba.empty());
}

TEST_CASE("ThumbnailCache: a real 16:9 video decodes at its own aspect, not MLT's default 4:3-ish profile "
         "(audit E2)")
{
    using namespace ustudio::core;

    sharedFactoryPolicy();
    std::random_device rd;
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("ustudio-thumbnail-e2-test-" + std::to_string(rd()) + ".mp4");
    RemoveOnExit cleanup{path};

    // Renders a tiny real 1920x1080 MP4 (doc 11: no binary media
    // committed, generated on the fly), same avformat/libx264 pair
    // renderProject() itself uses -- matches test_probe_media.cpp's own
    // pattern.
    {
        Model renderModel = Model::createEmpty();
        EngineSync renderSync(renderModel);
        Mlt::Producer producer(renderSync.profile(), "color:red");
        producer.set_in_and_out(0, 4);
        std::unique_ptr<Mlt::Profile> consumerProfile(producer.profile());
        Mlt::Consumer consumer(*consumerProfile, "avformat", path.string().c_str());
        consumer.set("vcodec", h264Encoder().c_str());
        consumer.connect(producer);
        consumer.run();
    }

    std::mutex mutex;
    int readyCount = 0;
    ThumbnailCache cache(
        testPool(),
        [&] {
            std::lock_guard<std::mutex> lock(mutex);
            ++readyCount;
        },
        2, ustudio::core::concurrency::Priority::Interactive);

    CHECK(cache.thumbnailFor(path.string()) == nullptr);
    bool ready = pumpMainContextUntil(
        [&] {
            std::lock_guard<std::mutex> lock(mutex);
            return readyCount >= 1;
        },
        std::chrono::seconds(5));
    REQUIRE(ready);

    const ThumbnailCache::Data *data = cache.thumbnailFor(path.string());
    REQUIRE(data != nullptr);
    CHECK(data->width == 120);
    // 120 * 1080 / 1920 == 67 (the source's real 16:9 shape). MLT's
    // default dv_pal profile (720x576, decoded before this fix) would
    // instead have produced 120 * 576 / 720 == 96 -- a visibly different,
    // wrong number, not just an off-by-one.
    CHECK(data->height == 67);

    // No letterbox bars: the very first row (where dv_pal's black bar
    // used to sit) must be the source's real red, not black.
    REQUIRE(data->rgba.size() >= 4);
    CHECK(data->rgba[0] > 200); // R
    CHECK(data->rgba[1] < 50);  // G
    CHECK(data->rgba[2] < 50);  // B
}

TEST_CASE("ThumbnailCache: frame thumbnails show the frame asked for, at the sequence's rate")
{
    sharedFactoryPolicy();
    // The A/V sync clip: white on frame 0 of every second, black between.
    ustudio::core::Model model = ustudio::core::Model::createEmpty();
    EngineSync sync(model);
    int fps = ustudio::testing::framesPerSecond(sync.profile());
    std::random_device rd;
    std::filesystem::path media =
        std::filesystem::temp_directory_path() / ("ustudio-frame-thumbs-" + std::to_string(rd()) + ".mp4");
    RemoveOnExit cleanup{media};
    ustudio::testing::renderSyncClip(sync.profile(), media.string(), 3);

    std::atomic<int> ready{0};
    ThumbnailCache cache(testPool(), [&] { ++ready; }, 2, ustudio::core::concurrency::Priority::Interactive);
    auto brightness = [&](int frame) -> int {
        const ThumbnailCache::Data *data = nullptr;
        pumpMainContextUntil([&] { return (data = cache.frameThumbnail(media.string(), frame, fps, 1)) != nullptr; },
                             std::chrono::seconds(10));
        REQUIRE(data);
        REQUIRE(!data->rgba.empty());
        const uint8_t *centre = data->rgba.data() + ((data->height / 2) * data->width + data->width / 2) * 4;
        return centre[0];
    };
    CHECK(brightness(fps) > 200);          // second 1's flash
    CHECK(brightness(fps + fps / 2) < 50); // half a second later
    CHECK(brightness(2 * fps) > 200);
}

// doc 12, M3: "Timeline stays responsive while thumbnails and waveforms
// generate". The timeline's snapshot only ever asks the caches; it must
// never wait for them. Hundreds of requests while both workers are busy
// decoding: every call returns at once.
TEST_CASE("ThumbnailCache and WaveformCache never block the caller while their workers decode")
{
    sharedFactoryPolicy();
    ustudio::core::Model model = ustudio::core::Model::createEmpty();
    EngineSync sync(model);
    int fps = ustudio::testing::framesPerSecond(sync.profile());
    std::random_device rd;
    std::filesystem::path media =
        std::filesystem::temp_directory_path() / ("ustudio-responsive-" + std::to_string(rd()) + ".mp4");
    RemoveOnExit cleanup{media};
    ustudio::testing::renderSyncClip(sync.profile(), media.string(), 4);

    ThumbnailCache thumbnails(testPool(), [] {}, 2, ustudio::core::concurrency::Priority::Interactive);
    WaveformCache waveforms(testPool(), [] {}, 2);
    double worstMs = 0.0;
    auto timed = [&](auto call) {
        auto start = std::chrono::steady_clock::now();
        call();
        worstMs = std::max(worstMs,
                           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    };
    for (int round = 0; round < 3; ++round) {
        for (int frame = 0; frame < 4 * fps; frame += 1)
            timed([&] { thumbnails.frameThumbnail(media.string(), frame, fps, 1); });
        for (int in = 0; in < 100; ++in)
            timed([&] { waveforms.peaksFor(media.string(), in, in + fps, ustudio::core::Rational{fps, 1}); });
        while (g_main_context_iteration(nullptr, FALSE)) {
        }
    }
    MESSAGE("slowest request while decoding: " << worstMs << " ms");
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__) // timings mean nothing there
    CHECK(worstMs < 5.0); // a frame at 60 Hz is 16 ms; a request must be a small part of one
#endif
}

TEST_CASE("ThumbnailCache: frame requests the view stopped asking for are dropped, not decoded (post-M3 audit P5)")
{
    sharedFactoryPolicy();
    ustudio::core::Model model = ustudio::core::Model::createEmpty();
    EngineSync sync(model);
    int fps = ustudio::testing::framesPerSecond(sync.profile());
    std::random_device rd;
    std::filesystem::path media =
        std::filesystem::temp_directory_path() / ("ustudio-stale-thumbs-" + std::to_string(rd()) + ".mp4");
    RemoveOnExit cleanup{media};
    ustudio::testing::renderSyncClip(sync.profile(), media.string(), 4);

    ThumbnailCache cache(testPool(), [] {}, 2, ustudio::core::concurrency::Priority::Interactive);
    // A zoom sweep: 30 rounds, each asking for a different set of 4 frames.
    for (int round = 0; round < 30; ++round) {
        cache.newFrameGeneration();
        for (int k = 0; k < 4; ++k)
            cache.frameThumbnail(media.string(), round + k * 30, fps, 1);
    }
    // Then the view settles: only the last round is still wanted.
    const ThumbnailCache::Data *settled = nullptr;
    pumpMainContextUntil(
        [&] {
            cache.newFrameGeneration();
            settled = cache.frameThumbnail(media.string(), 29 + 3 * 30, fps, 1);
            for (int k = 0; k < 3; ++k)
                cache.frameThumbnail(media.string(), 29 + k * 30, fps, 1);
            return settled != nullptr;
        },
        std::chrono::seconds(20));
    CHECK(settled);
    MESSAGE("decoded " << cache.frameThumbnailsDecoded() << " of 120 requested");
    CHECK(cache.frameThumbnailsDecoded() < 20);
}

TEST_CASE("ThumbnailCache and WaveformCache stay within their job caps and leave the pool room (doc 19 MT3)")
{
    sharedFactoryPolicy();
    // Eight files: eight batches, but at most two jobs at a time.
    ThumbnailCache thumbnails(testPool(), [] {}, 2, ustudio::core::concurrency::Priority::Interactive);
    WaveformCache waveforms(testPool(), [] {}, 1);
    std::vector<std::string> files;
    for (int i = 0; i < 8; ++i)
        files.push_back("noise:" + std::to_string(i));
    for (const std::string &file : files) {
        thumbnails.thumbnailFor(file);
        waveforms.peaksFor("tone:" + std::to_string(200 + 20 * static_cast<int>(files.size())), 0, 2'000,
                           ustudio::core::Rational{25, 1});
    }
    for (int i = 0; i < 6; ++i)
        waveforms.peaksFor("tone:" + std::to_string(300 + i), 0, 2'000, ustudio::core::Rational{25, 1});

    // A job of the pool's other users (an import's probe, a save) still gets
    // a thread while the caches have work queued.
    std::atomic<bool> otherRan{false};
    auto other =
        testPool().submit([&](std::stop_token) { otherRan = true; }, ustudio::core::concurrency::Priority::Interactive);
    other.wait();
    CHECK(otherRan);

    bool allReady = pumpMainContextUntil(
        [&] {
            for (const std::string &file : files)
                if (!thumbnails.thumbnailFor(file))
                    return false;
            for (int i = 0; i < 6; ++i)
                if (!waveforms.peaksFor("tone:" + std::to_string(300 + i), 0, 2'000, ustudio::core::Rational{25, 1}))
                    return false;
            return true;
        },
        std::chrono::seconds(60));
    CHECK(allReady);
    CAPTURE(thumbnails.peakConcurrentJobs());
    CAPTURE(waveforms.peakConcurrentJobs());
    CHECK(thumbnails.peakConcurrentJobs() >= 1);
    CHECK(thumbnails.peakConcurrentJobs() <= 2);
    CHECK(waveforms.peakConcurrentJobs() == 1);
}
