#include "render_queue.h"

#include <algorithm>
#include <utility>

namespace ustudio::app {

struct RenderQueue::Running
{
    RenderJob job;
    std::atomic<bool> cancel{false};
    bool cancelRequested = false; // main thread's view: cancelCurrent()/shutdown()
    bool succeeded = false;       // the worker's result; read after joining it
    std::thread thread;
};

RenderQueue::RenderQueue(Backend backend, Post post, Callbacks callbacks)
    : m_backend(std::move(backend)), m_post(std::move(post)), m_callbacks(std::move(callbacks))
{}

RenderQueue::~RenderQueue()
{
    shutdown();
}

uint64_t RenderQueue::enqueue(RenderJob job)
{
    job.id = m_nextId++;
    const uint64_t id = job.id;
    m_waiting.push_back(std::move(job));
    if (!m_running)
        startNext();
    return id;
}

const RenderJob *RenderQueue::running() const
{
    return m_running ? &m_running->job : nullptr;
}

bool RenderQueue::isQueued(const std::string &outputPath) const
{
    return std::any_of(m_waiting.begin(), m_waiting.end(),
                       [&](const RenderJob &job) { return job.outputPath == outputPath; });
}

void RenderQueue::startNext()
{
    if (m_shutDown || m_running || m_waiting.empty())
        return;
    m_running = std::make_unique<Running>();
    m_running->job = std::move(m_waiting.front());
    m_waiting.pop_front();
    Running *running = m_running.get();
    if (m_callbacks.started)
        m_callbacks.started(running->job);

    std::weak_ptr<void> token = m_lifetime;
    running->thread = std::thread([this, running, token] {
        std::string error;
        bool ok = m_backend(
            running->job, error,
            [this, running, token](int currentFrame, int totalFrames) {
                if (totalFrames <= 0)
                    return;
                const double fraction = static_cast<double>(currentFrame) / totalFrames;
                m_post([this, running, token, fraction] {
                    // Still this job? (A late progress can trail its finish.)
                    if (!token.expired() && m_running.get() == running && m_callbacks.progress)
                        m_callbacks.progress(running->job, fraction);
                });
            },
            running->cancel);
        running->succeeded = ok;
        m_post([this, running, token, ok, error] {
            if (!token.expired())
                onWorkerDone(running, ok, error);
        });
    });
}

void RenderQueue::onWorkerDone(Running *running, bool ok, const std::string &error)
{
    if (m_running.get() != running)
        return; // shutdown() already took it
    std::unique_ptr<Running> done = std::move(m_running);
    done->thread.join(); // it has returned from the backend already
    if (m_callbacks.finished)
        m_callbacks.finished(done->job, ok && !done->cancelRequested, done->cancelRequested, error);
    startNext();
}

void RenderQueue::cancelCurrent()
{
    if (!m_running)
        return;
    m_running->cancelRequested = true;
    m_running->cancel = true;
}

std::vector<RenderJob> RenderQueue::shutdown()
{
    std::vector<RenderJob> unfinished;
    if (m_shutDown)
        return unfinished;
    m_shutDown = true;
    if (m_running) {
        m_running->cancelRequested = true;
        m_running->cancel = true;
        m_running->thread.join();
        // It may have finished before the cancel reached it.
        if (!m_running->succeeded)
            unfinished.push_back(std::move(m_running->job));
        m_running.reset();
    }
    for (RenderJob &job : m_waiting)
        unfinished.push_back(std::move(job));
    m_waiting.clear();
    // Results the worker already posted find the token gone.
    m_lifetime.reset();
    return unfinished;
}

} // namespace ustudio::app
