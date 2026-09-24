#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace ustudio::core::concurrency {

// doc 19 / ADR-016's worker pool: work that must not run on the main thread
// (probing, save serialisation, thumbnails, analysis) runs here, on
// snapshots, and hands results back. std-only (core layer): no GLib, no MLT.

enum class Priority
{
    Interactive, // what the user is waiting on: a file just imported, a visible thumbnail
    Background,  // off-screen waveforms, autosave
};

enum class JobState
{
    Pending,
    Running,
    Done,
    Cancelled, // cancelled before it started, or stopped by request while running
};

namespace detail {
struct JobControl;
}

// Handle to a submitted job, shareable and cheap to copy. The job itself is
// cooperative: cancel() requests stop on its std::stop_token; a job that
// hasn't started yet never runs.
class JobHandle
{
  public:
    JobHandle() = default;
    bool valid() const
    {
        return static_cast<bool>(m_control);
    }
    JobState state() const;
    void cancel();
    // Blocks until the job is Done or Cancelled (tests, shutdown paths;
    // never from the main thread in the app).
    void wait() const;

  private:
    friend class ThreadPool;
    explicit JobHandle(std::shared_ptr<detail::JobControl> control) : m_control(std::move(control)) {}
    std::shared_ptr<detail::JobControl> m_control;
};

class ThreadPool
{
  public:
    using Job = std::function<void(std::stop_token)>;

    // max(2, hardware_concurrency / 2): measured on the dev machine, more
    // threads isn't automatically faster (doc 19, "Sizing").
    static size_t defaultThreadCount();

    explicit ThreadPool(size_t threads = defaultThreadCount());
    // Requests stop on every job, drops the queued ones (Cancelled, never
    // run), and joins the workers once the running jobs return.
    ~ThreadPool();
    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    // FIFO within a priority; Interactive jobs always go before Background.
    JobHandle submit(Job job, Priority priority = Priority::Background);

    size_t threadCount() const
    {
        return m_workers.size();
    }

  private:
    struct Queued
    {
        std::shared_ptr<detail::JobControl> control;
        Job job;
    };

    void workerMain(std::stop_token poolStop);

    std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::deque<Queued> m_interactive;
    std::deque<Queued> m_background;
    std::vector<std::shared_ptr<detail::JobControl>> m_running;
    bool m_shuttingDown = false;
    // Last, so they're joined first when the pool is destroyed.
    std::vector<std::jthread> m_workers;
};

} // namespace ustudio::core::concurrency
