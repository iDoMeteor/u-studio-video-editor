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
    // Runs a closure on the main thread after `ms` (g_timeout_add in the
    // app). Without one, probes never time out.
    using PostAfter = std::function<void(unsigned ms, std::function<void()>)>;

    // A probe still running `timeoutMs` after it started is given up
    // (doc 07): avformat can't be interrupted, so its thread finishes on
    // its own and the late result is dropped; the file is applied as
    // failed with `timedOut` set, and the files after it aren't held up.
    static constexpr unsigned kDefaultTimeoutMs = 20'000;
    ImportQueue(core::concurrency::ThreadPool &pool, Post post, PostAfter postAfter = {},
                unsigned timeoutMs = kDefaultTimeoutMs);

    void start(std::vector<std::string> paths, Probe probe, Apply apply, Progress progress = {},
               Finished finished = {});

    // Closing, opening or starting a new project: stops the probes and drops
    // any result still on its way. Main thread.
    void cancelAll();

    // Batches started and not yet finished or cancelled.
    size_t activeBatches() const;

    // What a timed-out probe is applied as.
    static Probed timedOut();

  private:
    struct Batch;
    void onStarted(const std::shared_ptr<Batch> &batch, size_t index);
    void onProbed(const std::shared_ptr<Batch> &batch, size_t index, Probed probed);
    void postApply(const std::shared_ptr<Batch> &batch);
    void applyNext(const std::shared_ptr<Batch> &batch);

    core::concurrency::ThreadPool &m_pool;
    Post m_post;
    PostAfter m_postAfter;
    unsigned m_timeoutMs;
    std::vector<std::shared_ptr<Batch>> m_batches;
};

// Import's file list with folders expanded (Import Folder…, a folder
// dropped from Files): every file under each folder, recursively, sorted,
// skipping hidden files and folders; files are kept as given. At most
// `limit` files; `truncated` says whether more were left out. IO: call it
// on the pool.
std::vector<std::string> expandImportPaths(const std::vector<std::string> &paths, size_t limit, bool *truncated);

} // namespace ustudio::app
