#pragma once

#include "core/concurrency/thread_pool.h"
#include "engine/engine_sync.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ustudio::app {

// doc 19 MT1: importing files probes them on the worker pool, in parallel,
// and applies the results on the main thread in the order the files were
// picked, not the order the probes finish, so clips still land one after
// another. No GTK here (the window supplies how to post to the main
// thread), so tests/app/test_import_queue.cpp drives it directly.
class ImportQueue
{
  public:
    using Probed = engine::EngineSync::ProbedMedia;
    // Runs on a pool thread: must not touch the live Model, widgets or
    // EngineSync (EngineSync::probeMediaFile with a copied profile).
    using Probe = std::function<Probed(const std::string &path)>;
    // Hands a closure to the main thread (MainThreadDispatcher::post in the
    // app, with the window's lifetime token).
    using Post = std::function<void(std::function<void()>)>;
    // Main thread, once per file, in the order given. A failed probe comes
    // through with length 0 for the caller to report and skip.
    using Apply = std::function<void(size_t index, const std::string &path, const Probed &probed)>;
    // Main thread, after each probe finishes: (finished, total).
    using Progress = std::function<void(size_t finished, size_t total)>;
    using Finished = std::function<void()>;

    ImportQueue(core::concurrency::ThreadPool &pool, Post post);

    void start(std::vector<std::string> paths, Probe probe, Apply apply, Progress progress = {},
               Finished finished = {});

    // Closing, opening or starting a new project: stops the probes and drops
    // any result still on its way. Main thread.
    void cancelAll();

    // Batches started and not yet finished or cancelled.
    size_t activeBatches() const;

  private:
    struct Batch;
    void onProbed(const std::shared_ptr<Batch> &batch, size_t index, Probed probed);
    void postApply(const std::shared_ptr<Batch> &batch);
    void applyNext(const std::shared_ptr<Batch> &batch);

    core::concurrency::ThreadPool &m_pool;
    Post m_post;
    std::vector<std::shared_ptr<Batch>> m_batches;
};

} // namespace ustudio::app
