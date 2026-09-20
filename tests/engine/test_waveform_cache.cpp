#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine/factory_policy.h"
#include "engine/waveform_cache.h"

#include <glib.h>

#include <chrono>
#include <mutex>
#include <thread>

using namespace ustudio::core;
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

// WaveformCache's onReady callback is marshaled onto the default GLib main
// context (g_idle_add) -- nothing runs that context's loop in a plain
// doctest binary, so tests must pump it themselves (same pattern as
// test_playback_controller.cpp's pumpMainContextUntil, duplicated locally
// rather than shared across two small, independent test binaries).
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

TEST_CASE("WaveformCache: computes peaks for a synthetic tone, one per requested frame")
{
    sharedFactoryPolicy();
    std::mutex mutex;
    int readyCount = 0;
    WaveformCache cache([&] {
        std::lock_guard<std::mutex> lock(mutex);
        ++readyCount;
    });

    // tone: is an MLT generator producer -- synthetic, no file (doc 11's
    // no-binary-media rule).
    const std::vector<float> *peaks = cache.peaksFor("tone:880", 0, 9, Rational{30, 1});
    CHECK(peaks == nullptr); // not computed yet, first call just queues the job

    bool ready = pumpMainContextUntil(
        [&] {
            std::lock_guard<std::mutex> lock(mutex);
            return readyCount > 0;
        },
        std::chrono::milliseconds(5000));
    REQUIRE(ready);

    peaks = cache.peaksFor("tone:880", 0, 9, Rational{30, 1});
    REQUIRE(peaks != nullptr);
    CHECK(peaks->size() == 10);
}

TEST_CASE("WaveformCache: the same resource/in/out at a different fps is a distinct cache entry (audit E5)")
{
    sharedFactoryPolicy();
    std::mutex mutex;
    int readyCount = 0;
    WaveformCache cache([&] {
        std::lock_guard<std::mutex> lock(mutex);
        ++readyCount;
    });

    // Two jobs differ only in fps -- E5's fix opens each job's throwaway
    // Producer at the SEQUENCE's fps (here, deliberately two different
    // ones), so a cache keyed only on (resource, in, out) would wrongly
    // hand back one rate's peaks for the other's request.
    CHECK(cache.peaksFor("tone:880", 0, 9, Rational{25, 1}) == nullptr);
    CHECK(cache.peaksFor("tone:880", 0, 9, Rational{50, 1}) == nullptr);

    bool bothReady = pumpMainContextUntil(
        [&] {
            std::lock_guard<std::mutex> lock(mutex);
            return readyCount >= 2;
        },
        std::chrono::milliseconds(5000));
    REQUIRE(bothReady);

    const std::vector<float> *at25 = cache.peaksFor("tone:880", 0, 9, Rational{25, 1});
    const std::vector<float> *at50 = cache.peaksFor("tone:880", 0, 9, Rational{50, 1});
    REQUIRE(at25 != nullptr);
    REQUIRE(at50 != nullptr);
    // Not the same cache entry: computed independently (address distinct),
    // each still covering the requested 10 frames regardless of fps.
    CHECK(at25 != at50);
    CHECK(at25->size() == 10);
    CHECK(at50->size() == 10);
}
