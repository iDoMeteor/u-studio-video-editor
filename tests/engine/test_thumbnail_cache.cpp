#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine/factory_policy.h"
#include "engine/thumbnail_cache.h"

#include <glib.h>

#include <chrono>
#include <mutex>
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
