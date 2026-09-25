#pragma once

#include "core/model/model.h"
#include "core/render/render_profile.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ustudio::app {

// One render, fixed when it's queued: what to render, how, and where.
struct RenderJob
{
    uint64_t id = 0; // set by RenderQueue::enqueue()
    std::shared_ptr<const core::Project> snapshot;
    core::RenderProfile profile;
    std::string outputPath;
};

// Renders one job at a time, in order, each on its own thread. The backend
// does the work -- engine::renderProject() today, the u-studio-render child
// process later (doc 19 MT5) -- so it's the one thing to swap. Main thread
// only, apart from the backend itself; results come back through `post`.
class RenderQueue
{
  public:
    // Worker thread. Returns false with `error` set on failure; a cancelled
    // render returns false once `cancel` reads true, having removed its
    // partial file.
    using Backend = std::function<bool(const RenderJob &job, std::string &error,
                                       std::function<void(int currentFrame, int totalFrames)> onProgress,
                                       const std::atomic<bool> &cancel)>;
    using Post = std::function<void(std::function<void()>)>;

    struct Callbacks
    {
        std::function<void(const RenderJob &job)> started;
        std::function<void(const RenderJob &job, double fraction)> progress;
        // `cancelled` when cancelCurrent() or shutdown() stopped it.
        std::function<void(const RenderJob &job, bool ok, bool cancelled, const std::string &error)> finished;
    };

    RenderQueue(Backend backend, Post post, Callbacks callbacks);
    ~RenderQueue(); // shutdown()

    // Starts it now if nothing is rendering, else queues it. Returns its id.
    uint64_t enqueue(RenderJob job);

    bool busy() const
    {
        return m_running != nullptr;
    }
    // Waiting behind the running one.
    size_t queued() const
    {
        return m_waiting.size();
    }
    const RenderJob *running() const;
    // A waiting job writes to `outputPath`.
    bool isQueued(const std::string &outputPath) const;

    // Stops the running job (its partial file is removed); the next queued
    // one starts once it has stopped.
    void cancelCurrent();

    // Quitting: stops the running job, waits for it, and returns it and the
    // queued ones, unfinished, in order. Nothing starts afterwards.
    std::vector<RenderJob> shutdown();

  private:
    struct Running;
    void startNext();
    void onWorkerDone(Running *running, bool ok, const std::string &error);

    Backend m_backend;
    Post m_post;
    Callbacks m_callbacks;
    std::unique_ptr<Running> m_running;
    std::deque<RenderJob> m_waiting;
    uint64_t m_nextId = 1;
    bool m_shutDown = false;
    // Outlives nothing it shouldn't: a worker's result posted after
    // shutdown() finds it expired and is dropped.
    std::shared_ptr<void> m_lifetime = std::make_shared<int>(0);
};

} // namespace ustudio::app
