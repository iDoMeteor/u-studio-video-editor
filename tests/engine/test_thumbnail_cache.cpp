#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/model/model.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"
#include "engine/thumbnail_cache.h"

#include <glib.h>
#include <mlt++/Mlt.h>

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

} // namespace

TEST_CASE("ThumbnailCache: computes a small RGBA image for a synthetic colour producer")
{
    sharedFactoryPolicy();
    std::mutex mutex;
    int readyCount = 0;
    ThumbnailCache cache([&] {
        std::lock_guard<std::mutex> lock(mutex);
        ++readyCount;
    });

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
    ThumbnailCache cache([&] {
        std::lock_guard<std::mutex> lock(mutex);
        ++readyCount;
    });

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
    ThumbnailCache cache([&] {
        std::lock_guard<std::mutex> lock(mutex);
        ++readyCount;
    });

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
    ThumbnailCache cache([&] {
        std::lock_guard<std::mutex> lock(mutex);
        ++readyCount;
    });

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
