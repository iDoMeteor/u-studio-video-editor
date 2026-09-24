#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/save_queue.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <latch>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace ustudio;
using app::SaveQueue;

namespace {

// Stands in for the GTK main thread (as in test_import_queue.cpp): posts
// queue here and run on the test's thread. Waits are on conditions.
struct FakeMain
{
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> posted;

    SaveQueue::Post post()
    {
        return [this](std::function<void()> fn) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                posted.push_back(std::move(fn));
            }
            cv.notify_all();
        };
    }
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
};

// The writes a test ran, in order, from whichever thread ran them.
struct Log
{
    std::mutex mutex;
    std::vector<std::string> writes;
    void add(const std::string &name)
    {
        std::lock_guard<std::mutex> lock(mutex);
        writes.push_back(name);
    }
    std::vector<std::string> get()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return writes;
    }
};

} // namespace

TEST_CASE("SaveQueue: autosave is latest-wins, one running and one waiting")
{
    core::concurrency::ThreadPool pool(2);
    FakeMain main;
    SaveQueue queue(pool, main.post());
    Log log;
    std::latch release(1);
    std::vector<std::string> done;
    auto write = [&](std::string name, bool block) {
        return [&log, &release, name, block] {
            if (block)
                release.wait();
            log.add(name);
            return std::string{};
        };
    };
    queue.autosave(write("a1", true), [&](const std::string &) { done.push_back("a1"); });
    queue.autosave(write("a2", false), [&](const std::string &) { done.push_back("a2"); });
    queue.autosave(write("a3", false), [&](const std::string &) { done.push_back("a3"); });
    CHECK(queue.busy());
    release.count_down();
    main.runUntil([&] { return done.size() == 2; });
    CHECK(log.get() == std::vector<std::string>{"a1", "a3"}); // a2 was superseded
    CHECK(done == std::vector<std::string>{"a1", "a3"});
    CHECK_FALSE(queue.busy());
}

TEST_CASE("SaveQueue: a newer save replaces a waiting one; both callers hear its result")
{
    core::concurrency::ThreadPool pool(2);
    FakeMain main;
    SaveQueue queue(pool, main.post());
    Log log;
    std::latch release(1);
    std::vector<std::string> done;
    queue.autosave(
        [&] {
            release.wait();
            log.add("auto");
            return std::string{};
        },
        {});
    queue.save(
        [&] {
            log.add("s1");
            return std::string{};
        },
        [&](const std::string &error) { done.push_back("s1:" + error); });
    queue.save(
        [&] {
            log.add("s2");
            return std::string("disk full");
        },
        [&](const std::string &error) { done.push_back("s2:" + error); });
    release.count_down();
    main.runUntil([&] { return done.size() == 2; });
    CHECK(log.get() == std::vector<std::string>{"auto", "s2"});
    CHECK(done == std::vector<std::string>{"s1:disk full", "s2:disk full"});
}

TEST_CASE("SaveQueue: a waiting save goes before a waiting autosave, and writes never overlap")
{
    core::concurrency::ThreadPool pool(4);
    FakeMain main;
    SaveQueue queue(pool, main.post());
    Log log;
    std::latch release(1);
    std::atomic<int> active{0};
    std::atomic<int> maxActive{0};
    int done = 0;
    auto write = [&](std::string name, bool block) {
        return [&, name, block] {
            int now = ++active;
            maxActive = std::max(maxActive.load(), now);
            if (block)
                release.wait();
            log.add(name);
            --active;
            return std::string{};
        };
    };
    queue.save(write("s1", true), [&](const std::string &) { ++done; });
    queue.autosave(write("a1", false), [&](const std::string &) { ++done; });
    queue.save(write("s2", false), [&](const std::string &) { ++done; });
    release.count_down();
    main.runUntil([&] { return done == 3; });
    CHECK(log.get() == std::vector<std::string>{"s1", "s2", "a1"});
    CHECK(maxActive == 1);
}

TEST_CASE("SaveQueue: finish() waits for the running write and runs the waiting ones (quit)")
{
    core::concurrency::ThreadPool pool(2);
    FakeMain main;
    Log log;
    std::latch started(1);
    std::latch release(1);
    std::vector<std::string> done;
    {
        SaveQueue queue(pool, main.post());
        queue.save(
            [&] {
                started.count_down();
                release.wait();
                log.add("s1");
                return std::string{};
            },
            [&](const std::string &) { done.push_back("s1"); });
        queue.autosave(
            [&] {
                log.add("a1");
                return std::string{};
            },
            [&](const std::string &) { done.push_back("a1"); });
        started.wait();
        std::thread releaser([&] { release.count_down(); });
        queue.finish(); // blocks until s1 is written, then writes a1 inline
        releaser.join();
        CHECK(log.get() == std::vector<std::string>{"s1", "a1"});
        CHECK(done == std::vector<std::string>{"s1", "a1"});
        CHECK_FALSE(queue.busy());
    }
    // The queue is gone; s1's own posted completion (posted before the job
    // returned, so it's queued by now) must not run anything.
    {
        std::deque<std::function<void()>> left;
        {
            std::lock_guard<std::mutex> lock(main.mutex);
            left.swap(main.posted);
        }
        for (auto &fn : left)
            fn();
    }
    CHECK(done.size() == 2);
}
