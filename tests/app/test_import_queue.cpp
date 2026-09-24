#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/import_queue.h"
#include "core/model/model.h"
#include "engine/factory_policy.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <latch>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace ustudio;
using app::ImportQueue;

namespace {

// Stands in for the GTK main thread: posts queue here, the test drains them
// on its own thread. Waits are on conditions, never sleeps.
struct FakeMain
{
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> posted;
    size_t postCount = 0;

    ImportQueue::Post post()
    {
        return [this](std::function<void()> fn) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                posted.push_back(std::move(fn));
                ++postCount;
            }
            cv.notify_all();
        };
    }
    // Runs posted closures on this thread until `done()` holds, waiting for
    // the next post whenever the queue is empty.
    void runUntil(const std::function<bool()> &done)
    {
        std::unique_lock<std::mutex> lock(mutex);
        while (!done()) {
            if (posted.empty()) {
                cv.wait(lock, [&] { return !posted.empty(); });
                continue;
            }
            auto fn = std::move(posted.front());
            posted.pop_front();
            lock.unlock();
            fn();
            lock.lock();
        }
    }
    void waitForPosts(size_t n)
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return postCount >= n; });
    }
    // One GLib main-loop iteration: runs what was queued when it began;
    // anything posted meanwhile waits for the next one.
    void runIteration()
    {
        std::deque<std::function<void()>> batch;
        {
            std::lock_guard<std::mutex> lock(mutex);
            batch.swap(posted);
        }
        for (auto &fn : batch)
            fn();
    }
};

ImportQueue::Probed ok(int length)
{
    ImportQueue::Probed p;
    p.length = length;
    return p;
}

} // namespace

TEST_CASE("ImportQueue: results apply in pick order however the probes finish")
{
    core::concurrency::ThreadPool pool(5); // one per probe, or the gated ones starve the rest
    FakeMain main;
    ImportQueue queue(pool, main.post());

    // Five probes that finish in reverse order: each waits for its gate, and
    // the test opens them 4, 3, 2, 1, 0.
    std::vector<std::unique_ptr<std::latch>> gates;
    for (int i = 0; i < 5; ++i)
        gates.push_back(std::make_unique<std::latch>(1));
    std::vector<std::string> paths{"f0", "f1", "f2", "f3", "f4"};
    std::vector<size_t> applied;
    std::vector<size_t> progress;
    bool finished = false;
    queue.start(
        paths,
        [&](const std::string &path) {
            int i = path[1] - '0';
            gates[static_cast<size_t>(i)]->wait();
            return ok(100 + i);
        },
        [&](size_t index, const std::string &path, const ImportQueue::Probed &probed) {
            CHECK(path == paths[index]);
            CHECK(probed.length == static_cast<core::FrameIndex>(100 + index));
            applied.push_back(index);
        },
        [&](size_t done, size_t total) {
            CHECK(total == 5);
            progress.push_back(done);
        },
        [&] { finished = true; });

    for (int i = 4; i >= 0; --i) {
        gates[static_cast<size_t>(i)]->count_down();
        main.runUntil([&] { return progress.size() == static_cast<size_t>(5 - i); });
        if (i > 0)
            CHECK(applied.empty()); // f0 isn't in yet, so nothing can apply
    }
    main.runUntil([&] { return finished; });
    CHECK(applied == std::vector<size_t>{0, 1, 2, 3, 4});
    CHECK(progress == std::vector<size_t>{1, 2, 3, 4, 5});
    CHECK(finished);
    CHECK(queue.activeBatches() == 0);
}

TEST_CASE("ImportQueue: a failed probe comes through in its place and doesn't hold up the rest")
{
    core::concurrency::ThreadPool pool(2);
    FakeMain main;
    ImportQueue queue(pool, main.post());
    std::vector<std::pair<size_t, core::FrameIndex>> applied;
    queue.start(
        {"good", "bad", "good2"}, [](const std::string &path) { return path == "bad" ? ok(0) : ok(50); },
        [&](size_t index, const std::string &, const ImportQueue::Probed &probed) {
            applied.emplace_back(index, probed.length);
        });
    main.runUntil([&] { return applied.size() == 3; });
    CHECK(applied == std::vector<std::pair<size_t, core::FrameIndex>>{{0, 50}, {1, 0}, {2, 50}});
}

TEST_CASE("ImportQueue: results already in are applied one per main-loop iteration")
{
    core::concurrency::ThreadPool pool(4);
    FakeMain main;
    ImportQueue queue(pool, main.post());
    std::vector<size_t> applied;
    std::vector<size_t> progress;
    queue.start(
        {"a", "b", "c", "d"}, [](const std::string &) { return ok(10); },
        [&](size_t index, const std::string &, const ImportQueue::Probed &) { applied.push_back(index); },
        [&](size_t done, size_t) { progress.push_back(done); });
    main.waitForPosts(4); // every probe is in before the loop runs at all
    main.runIteration();  // all four results recorded; the first apply is posted
    CHECK(progress.size() == 4);
    CHECK(applied.empty());
    for (size_t expected = 1; expected <= 4; ++expected) {
        main.runIteration();
        CHECK(applied.size() == expected);
    }
    CHECK(queue.activeBatches() == 0);
}

TEST_CASE("ImportQueue: cancelling drops everything still on its way")
{
    std::optional<core::concurrency::ThreadPool> pool(std::in_place, 2);
    FakeMain main;
    ImportQueue queue(*pool, main.post());
    std::latch running(2);
    std::latch release(1);
    std::atomic<int> probes{0};
    int applied = 0;
    queue.start(
        {"a", "b", "c", "d"},
        [&](const std::string &) {
            ++probes;
            running.count_down();
            release.wait();
            return ok(10);
        },
        [&](size_t, const std::string &, const ImportQueue::Probed &) { ++applied; });
    running.wait();    // two probes are mid-flight, two are queued
    queue.cancelAll(); // the project closed under them
    CHECK(queue.activeBatches() == 0);
    release.count_down();
    pool.reset();               // join: every late post has happened by now
    CHECK(main.postCount == 0); // the running probes saw the cancel and didn't post
    CHECK(applied == 0);
    CHECK(probes == 2); // the queued ones never started
}

TEST_CASE("ImportQueue: real probes of generated media run on the pool, in order")
{
    static engine::FactoryPolicy policy;
    core::concurrency::ThreadPool pool(4);
    FakeMain main;
    ImportQueue queue(pool, main.post());
    core::Profile profile = core::Model::createEmpty().sequence().profile;
    std::vector<std::string> paths{"color:red", "tone:", "/nonexistent/clip.mp4", "color:blue"};
    std::vector<ImportQueue::Probed> results(paths.size());
    std::vector<size_t> order;
    queue.start(
        paths, [profile](const std::string &path) { return engine::EngineSync::probeMediaFile(profile, path); },
        [&](size_t index, const std::string &, const ImportQueue::Probed &probed) {
            order.push_back(index);
            results[index] = probed;
        });
    main.runUntil([&] { return order.size() == paths.size(); });
    CHECK(order == std::vector<size_t>{0, 1, 2, 3});
    CHECK(results[0].length > 0);
    CHECK(results[1].length > 0);
    // (no hasAudio check: it reads avformat's audio_index, which generators lack)
    CHECK(results[2].length == 0); // failed, reported in its place
    CHECK(results[3].length > 0);
}
