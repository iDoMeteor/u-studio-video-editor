#include "thread_pool.h"

#include "core/log.h"

#include <algorithm>
#include <exception>

namespace ustudio::core::concurrency {

namespace detail {

struct JobControl
{
    std::atomic<JobState> state{JobState::Pending};
    std::stop_source stop;
    std::mutex mutex;
    std::condition_variable finished;

    // Pending -> Cancelled, if nothing has started it yet.
    bool cancelIfPending()
    {
        JobState expected = JobState::Pending;
        if (!state.compare_exchange_strong(expected, JobState::Cancelled))
            return false;
        std::lock_guard<std::mutex> lock(mutex);
        finished.notify_all();
        return true;
    }

    void finish(JobState end)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            state = end;
        }
        finished.notify_all();
    }
};

} // namespace detail

JobState JobHandle::state() const
{
    return m_control ? m_control->state.load() : JobState::Cancelled;
}

void JobHandle::cancel()
{
    if (!m_control)
        return;
    m_control->stop.request_stop();
    m_control->cancelIfPending();
}

void JobHandle::wait() const
{
    if (!m_control)
        return;
    std::unique_lock<std::mutex> lock(m_control->mutex);
    m_control->finished.wait(lock, [this] {
        JobState s = m_control->state.load();
        return s == JobState::Done || s == JobState::Cancelled;
    });
}

size_t ThreadPool::defaultThreadCount()
{
    return std::max<size_t>(2, std::thread::hardware_concurrency() / 2);
}

ThreadPool::ThreadPool(size_t threads)
{
    threads = std::max<size_t>(1, threads);
    m_workers.reserve(threads);
    for (size_t i = 0; i < threads; ++i)
        m_workers.emplace_back([this](std::stop_token stop) { workerMain(stop); });
}

ThreadPool::~ThreadPool()
{
    std::deque<Queued> dropped;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shuttingDown = true;
        dropped.swap(m_interactive);
        dropped.insert(dropped.end(), std::make_move_iterator(m_background.begin()),
                       std::make_move_iterator(m_background.end()));
        m_background.clear();
        for (const auto &running : m_running)
            running->stop.request_stop();
    }
    for (Queued &queued : dropped) {
        queued.control->stop.request_stop();
        queued.control->cancelIfPending();
    }
    m_wake.notify_all();
    // std::jthread's destructor requests stop and joins; the workers finish
    // whatever job they're in (which has been asked to stop) and exit.
    m_workers.clear();
}

JobHandle ThreadPool::submit(Job job, Priority priority)
{
    auto control = std::make_shared<detail::JobControl>();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shuttingDown) {
            control->state = JobState::Cancelled;
            return JobHandle(control);
        }
        Queued queued{control, std::move(job)};
        if (priority == Priority::Interactive)
            m_interactive.push_back(std::move(queued));
        else
            m_background.push_back(std::move(queued));
    }
    m_wake.notify_one();
    return JobHandle(control);
}

void ThreadPool::workerMain(std::stop_token poolStop)
{
    while (true) {
        Queued next;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, poolStop,
                        [this] { return m_shuttingDown || !m_interactive.empty() || !m_background.empty(); });
            if (poolStop.stop_requested() || m_shuttingDown)
                return;
            std::deque<Queued> &queue = !m_interactive.empty() ? m_interactive : m_background;
            next = std::move(queue.front());
            queue.pop_front();
            // A job cancelled while queued is skipped, never run.
            JobState expected = JobState::Pending;
            if (!next.control->state.compare_exchange_strong(expected, JobState::Running))
                continue;
            m_running.push_back(next.control);
        }

        std::stop_token token = next.control->stop.get_token();
        try {
            next.job(token);
        } catch (const std::exception &e) {
            Log::warn(std::string("[pool] a job threw: ") + e.what());
        } catch (...) {
            Log::warn("[pool] a job threw a non-exception");
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::erase(m_running, next.control);
        }
        next.control->finish(token.stop_requested() ? JobState::Cancelled : JobState::Done);
    }
}

} // namespace ustudio::core::concurrency
