#include "import_queue.h"

#include "core/trace.h"

#include <algorithm>
#include <atomic>

namespace ustudio::app {

struct ImportQueue::Batch
{
    std::vector<std::string> paths;
    Apply apply;
    Progress progress;
    Finished finished;
    std::vector<std::optional<Probed>> results; // main thread only
    size_t nextToApply = 0;                     // main thread only
    size_t probed = 0;                          // main thread only
    bool applyPosted = false;                   // main thread only
    std::atomic<bool> cancelled{false};
    std::vector<core::concurrency::JobHandle> jobs;
};

ImportQueue::ImportQueue(core::concurrency::ThreadPool &pool, Post post) : m_pool(pool), m_post(std::move(post)) {}

void ImportQueue::start(std::vector<std::string> paths, Probe probe, Apply apply, Progress progress, Finished finished)
{
    if (paths.empty())
        return;
    auto batch = std::make_shared<Batch>();
    batch->paths = std::move(paths);
    batch->apply = std::move(apply);
    batch->progress = std::move(progress);
    batch->finished = std::move(finished);
    batch->results.resize(batch->paths.size());
    m_batches.push_back(batch);

    for (size_t i = 0; i < batch->paths.size(); ++i) {
        batch->jobs.push_back(m_pool.submit(
            // Pool thread: only the batch's immutable paths, its atomic
            // flag and a copy of the post function; the queue itself is
            // touched only by the closure, back on the main thread.
            [this, post = m_post, batch, i, probe](std::stop_token stop) {
                if (stop.stop_requested() || batch->cancelled)
                    return;
                Probed result = probe(batch->paths[i]);
                if (stop.stop_requested() || batch->cancelled)
                    return;
                post([this, batch, i, result] { onProbed(batch, i, result); });
            },
            core::concurrency::Priority::Interactive));
    }
}

void ImportQueue::onProbed(const std::shared_ptr<Batch> &batch, size_t index, Probed probed)
{
    if (batch->cancelled)
        return; // the project it was for is gone
    batch->results[index] = std::move(probed);
    ++batch->probed;
    if (batch->progress)
        batch->progress(batch->probed, batch->paths.size());
    postApply(batch);
}

void ImportQueue::postApply(const std::shared_ptr<Batch> &batch)
{
    // In pick order: a file that finished early waits for the ones before it.
    if (batch->applyPosted || batch->nextToApply >= batch->results.size() || !batch->results[batch->nextToApply])
        return;
    // One file per main-loop iteration, re-posted rather than looped: each
    // apply is a command whose engine rebuild costs ~240 ms with a dozen
    // real clips (measured, doc 19), and GLib dispatches every ready idle
    // source of one priority in the same iteration, so applying straight
    // from onProbed() blocked the loop 2.4 s on 12 files. A source added
    // during dispatch waits for the next iteration, so input and redraws
    // get a turn between files.
    batch->applyPosted = true;
    m_post([this, batch] { applyNext(batch); });
}

void ImportQueue::applyNext(const std::shared_ptr<Batch> &batch)
{
    batch->applyPosted = false;
    if (batch->cancelled)
        return;
    core::trace::Scope trace("import: apply");
    size_t i = batch->nextToApply++;
    if (batch->apply)
        batch->apply(i, batch->paths[i], *batch->results[i]);
    if (batch->nextToApply == batch->results.size()) {
        std::erase(m_batches, batch);
        if (batch->finished)
            batch->finished();
        return;
    }
    postApply(batch);
}

void ImportQueue::cancelAll()
{
    for (const std::shared_ptr<Batch> &batch : m_batches) {
        batch->cancelled = true;
        for (core::concurrency::JobHandle &job : batch->jobs)
            job.cancel();
    }
    m_batches.clear();
}

size_t ImportQueue::activeBatches() const
{
    return m_batches.size();
}

} // namespace ustudio::app
