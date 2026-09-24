#include "save_queue.h"

#include "core/trace.h"

#include <utility>

namespace ustudio::app {

struct SaveQueue::Op
{
    Write write;
    std::vector<Done> dones;
    core::concurrency::Priority priority;
    std::string error; // set by the write, read after it
    core::concurrency::JobHandle job;
    bool completed = false; // main thread only: dones already called
};

SaveQueue::SaveQueue(core::concurrency::ThreadPool &pool, Post post) : m_pool(pool), m_post(std::move(post)) {}

SaveQueue::~SaveQueue() = default;

void SaveQueue::save(Write write, Done done)
{
    if (m_pendingSave) {
        // The newer snapshot supersedes the waiting one; its callers still
        // hear how the write went (a close-after-save among them).
        m_pendingSave->write = std::move(write);
        m_pendingSave->dones.push_back(std::move(done));
        return;
    }
    auto op = std::make_shared<Op>();
    op->write = std::move(write);
    op->dones.push_back(std::move(done));
    op->priority = core::concurrency::Priority::Interactive;
    if (m_running) {
        m_pendingSave = std::move(op);
        return;
    }
    submit(std::move(op));
}

void SaveQueue::autosave(Write write, Done done)
{
    auto op = std::make_shared<Op>();
    op->write = std::move(write);
    op->dones.push_back(std::move(done));
    op->priority = core::concurrency::Priority::Background;
    if (m_running) {
        m_pendingAutosave = std::move(op); // an older waiting one is dropped
        return;
    }
    submit(std::move(op));
}

bool SaveQueue::busy() const
{
    return m_running || m_pendingSave || m_pendingAutosave;
}

void SaveQueue::submit(std::shared_ptr<Op> op)
{
    m_running = op;
    // Captures the op and a copy of the post function, never `this`, on
    // the pool side; the closure runs on the main thread. A write isn't
    // cancellable: a half-done one would leave its ".tmp" behind.
    op->job = m_pool.submit(
        [this, op, post = m_post](std::stop_token) {
            {
                core::trace::Scope trace("save: write");
                op->error = op->write();
            }
            // After finish() the op is completed and the queue may be gone:
            // check before touching `this`.
            post([this, op] {
                if (!op->completed)
                    complete(op);
            });
        },
        op->priority);
}

void SaveQueue::complete(const std::shared_ptr<Op> &op)
{
    op->completed = true;
    if (m_running == op)
        m_running.reset();
    startNext();
    for (Done &done : op->dones)
        if (done)
            done(op->error);
}

void SaveQueue::startNext()
{
    if (m_running)
        return;
    if (m_pendingSave)
        submit(std::exchange(m_pendingSave, nullptr));
    else if (m_pendingAutosave)
        submit(std::exchange(m_pendingAutosave, nullptr));
}

void SaveQueue::finish()
{
    // Waiting writes run inline, not through the pool, which is about to be
    // destroyed; every Done is called here. A Done that queues another
    // write is picked up on the next pass.
    while (true) {
        std::shared_ptr<Op> op;
        if (m_running) {
            op = std::exchange(m_running, nullptr);
            op->job.wait();
        } else if (m_pendingSave || m_pendingAutosave) {
            op = m_pendingSave ? std::exchange(m_pendingSave, nullptr) : std::exchange(m_pendingAutosave, nullptr);
            op->error = op->write();
        } else {
            return;
        }
        op->completed = true; // the closure it may have posted finds it done
        for (Done &done : op->dones)
            if (done)
                done(op->error);
    }
}

} // namespace ustudio::app
