#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/concurrency/thread_pool.h"

#include <atomic>
#include <latch>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace ustudio::core::concurrency;

// Every wait here is on a condition (a latch, a handle, a join), never a
// sleep (doc 19: tests wait on conditions).

namespace {

// Occupies a one-thread pool until release() is called, so jobs submitted
// meanwhile queue up in a known state.
struct Gate
{
    std::latch started{1};
    std::latch release{1};
    JobHandle handle;
    explicit Gate(ThreadPool &pool)
    {
        handle = pool.submit(
            [this](std::stop_token) {
                started.count_down();
                release.wait();
            },
            Priority::Interactive);
        started.wait();
    }
    void open()
    {
        release.count_down();
        handle.wait();
    }
};

} // namespace

TEST_CASE("ThreadPool: default size is half the hardware threads, at least two")
{
    CHECK(ThreadPool::defaultThreadCount() >= 2);
    ThreadPool pool(3);
    CHECK(pool.threadCount() == 3);
}

TEST_CASE("ThreadPool: interactive jobs run before background ones, FIFO within each")
{
    ThreadPool pool(1);
    Gate gate(pool);
    std::mutex mutex;
    std::vector<std::string> order;
    auto record = [&](std::string name) {
        return [&, name](std::stop_token) {
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(name);
        };
    };
    std::vector<JobHandle> handles{
        pool.submit(record("bg1"), Priority::Background), pool.submit(record("bg2"), Priority::Background),
        pool.submit(record("ui1"), Priority::Interactive), pool.submit(record("ui2"), Priority::Interactive)};
    CHECK(handles[0].state() == JobState::Pending);
    gate.open();
    for (JobHandle &h : handles)
        h.wait();
    CHECK(order == std::vector<std::string>{"ui1", "ui2", "bg1", "bg2"});
    for (JobHandle &h : handles)
        CHECK(h.state() == JobState::Done);
}

TEST_CASE("ThreadPool: a job cancelled before it starts never runs")
{
    ThreadPool pool(1);
    Gate gate(pool);
    std::atomic<bool> ran{false};
    JobHandle job = pool.submit([&](std::stop_token) { ran = true; });
    JobHandle after = pool.submit([](std::stop_token) {});
    job.cancel();
    CHECK(job.state() == JobState::Cancelled);
    gate.open();
    after.wait(); // runs after the cancelled one would have
    job.wait();
    CHECK_FALSE(ran);
    CHECK(job.state() == JobState::Cancelled);
    CHECK(after.state() == JobState::Done);
}

TEST_CASE("ThreadPool: cancelling a running job stops it cooperatively")
{
    ThreadPool pool(2);
    std::latch running{1};
    std::atomic<bool> sawStop{false};
    JobHandle job = pool.submit([&](std::stop_token stop) {
        running.count_down();
        std::mutex m;
        std::condition_variable_any cv;
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, stop, [] { return false; }); // until stop is requested
        sawStop = stop.stop_requested();
    });
    running.wait();
    CHECK(job.state() == JobState::Running);
    job.cancel();
    job.wait();
    CHECK(sawStop);
    CHECK(job.state() == JobState::Cancelled);
}

TEST_CASE("ThreadPool: destruction stops running jobs, drops queued ones, and joins")
{
    std::atomic<int> stoppedRunning{0};
    std::atomic<int> ranQueued{0};
    std::vector<JobHandle> running, queued;
    {
        ThreadPool pool(2);
        std::latch bothRunning{2};
        for (int i = 0; i < 2; ++i) {
            running.push_back(pool.submit([&](std::stop_token stop) {
                bothRunning.count_down();
                std::mutex m;
                std::condition_variable_any cv;
                std::unique_lock<std::mutex> lock(m);
                cv.wait(lock, stop, [] { return false; });
                ++stoppedRunning;
            }));
        }
        bothRunning.wait();
        for (int i = 0; i < 10; ++i)
            queued.push_back(pool.submit([&](std::stop_token) { ++ranQueued; }));
    } // ~ThreadPool
    CHECK(stoppedRunning == 2);
    CHECK(ranQueued == 0);
    for (JobHandle &h : running)
        CHECK(h.state() == JobState::Cancelled);
    for (JobHandle &h : queued)
        CHECK(h.state() == JobState::Cancelled);
}

TEST_CASE("ThreadPool: thousands of tiny jobs from several producer threads each run at most once")
{
    ThreadPool pool;
    constexpr int kProducers = 8, kJobsEach = 2000;
    // One counter per job: a job cancelled while it was already running may
    // still have run to the end (it's reported Cancelled, since stop was
    // requested), so the check is per job, not a total.
    std::vector<std::atomic<int>> runs(kProducers * kJobsEach);
    std::vector<std::vector<JobHandle>> handles(kProducers);
    {
        std::vector<std::jthread> producers;
        for (int p = 0; p < kProducers; ++p) {
            producers.emplace_back([&, p] {
                for (int j = 0; j < kJobsEach; ++j) {
                    std::atomic<int> &slot = runs[static_cast<size_t>(p * kJobsEach + j)];
                    handles[static_cast<size_t>(p)].push_back(
                        pool.submit([&slot](std::stop_token) { slot.fetch_add(1, std::memory_order_relaxed); },
                                    j % 3 == 0 ? Priority::Interactive : Priority::Background));
                    if (j % 97 == 0) // and some cancelled along the way
                        handles[static_cast<size_t>(p)].back().cancel();
                }
            });
        }
    }
    int done = 0, cancelled = 0, twice = 0, doneNotRun = 0;
    for (int p = 0; p < kProducers; ++p) {
        for (int j = 0; j < kJobsEach; ++j) {
            JobHandle &h = handles[static_cast<size_t>(p)][static_cast<size_t>(j)];
            h.wait();
            int ran = runs[static_cast<size_t>(p * kJobsEach + j)].load();
            twice += ran > 1;
            if (h.state() == JobState::Done) {
                ++done;
                doneNotRun += ran != 1;
            } else {
                ++cancelled;
            }
        }
    }
    CHECK(done + cancelled == kProducers * kJobsEach);
    CHECK(twice == 0);
    CHECK(doneNotRun == 0);
    CHECK(cancelled <= kProducers * (kJobsEach / 97 + 1));
}

TEST_CASE("ThreadPool: a job that throws ends Failed, is logged, and doesn't take the worker down")
{
    ThreadPool pool(1);
    JobHandle bad = pool.submit([](std::stop_token) { throw std::runtime_error("boom"); });
    JobHandle good = pool.submit([](std::stop_token) {});
    bad.wait();
    good.wait();
    CHECK(bad.state() == JobState::Failed);
    CHECK(good.state() == JobState::Done);
}
