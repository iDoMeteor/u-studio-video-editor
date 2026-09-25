#include "import_queue.h"

#include "core/media/utf8_path.h"
#include "core/trace.h"

#include <algorithm>
#include <atomic>
#include <filesystem>

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

ImportQueue::ImportQueue(core::concurrency::ThreadPool &pool, Post post, PostAfter postAfter, unsigned timeoutMs)
    : m_pool(pool), m_post(std::move(post)), m_postAfter(std::move(postAfter)), m_timeoutMs(timeoutMs)
{}

ImportQueue::Probed ImportQueue::timedOut()
{
    Probed probed;
    probed.error = "it took too long to open (over 20 s)";
    return probed;
}

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
            [this, post = m_post, batch, i, probe, timed = bool(m_postAfter)](std::stop_token stop) {
                if (stop.stop_requested() || batch->cancelled)
                    return;
                if (timed)
                    post([this, batch, i] { onStarted(batch, i); });
                Probed result = probe(batch->paths[i]);
                if (stop.stop_requested() || batch->cancelled)
                    return;
                post([this, batch, i, result] { onProbed(batch, i, result); });
            },
            core::concurrency::Priority::Interactive));
    }
}

void ImportQueue::onStarted(const std::shared_ptr<Batch> &batch, size_t index)
{
    if (batch->cancelled)
        return;
    m_postAfter(m_timeoutMs, [this, batch, index] {
        if (!batch->cancelled && !batch->results[index])
            onProbed(batch, index, timedOut());
    });
}

void ImportQueue::onProbed(const std::shared_ptr<Batch> &batch, size_t index, Probed probed)
{
    if (batch->cancelled || batch->results[index])
        return; // the project it was for is gone, or it timed out already
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

std::vector<std::string> expandImportPaths(const std::vector<std::string> &paths, size_t limit, bool *truncated)
{
    namespace fs = std::filesystem;
    std::vector<std::string> files;
    bool more = false;
    auto add = [&](std::string path) {
        if (files.size() >= limit) {
            more = true;
            return false;
        }
        files.push_back(std::move(path));
        return true;
    };
    for (const std::string &path : paths) {
        std::error_code ec;
        const fs::path root = core::pathFromUtf8(path);
        if (!fs::is_directory(root, ec)) {
            if (!add(path))
                break;
            continue;
        }
        std::vector<std::string> found;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            if (core::utf8String(it->path().filename()).starts_with(".")) {
                if (it->is_directory(ec))
                    it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file(ec))
                found.push_back(core::utf8String(it->path()));
        }
        std::sort(found.begin(), found.end());
        for (std::string &file : found)
            if (!add(std::move(file)))
                break;
        if (more)
            break;
    }
    if (truncated)
        *truncated = more;
    return files;
}

} // namespace ustudio::app
