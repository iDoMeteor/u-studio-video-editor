#pragma once

// Renders the canvas's title off the main thread (CLAUDE.md: blocking work
// leaves the main thread): one worker, latest request wins, results back on
// the main thread through GLib's main loop. A 4K title takes ~14 ms, a
// canvas-sized one a few; while the user drags, only the newest document
// is drawn.

#include "render/title_renderer.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace ustudio::titles::app {

class RenderWorker
{
  public:
    // Main thread, with the frame for the newest request (older ones are
    // dropped). Never called after the worker is destroyed.
    using Done = std::function<void(RenderResult result, uint64_t generation)>;

    explicit RenderWorker(Done done);
    ~RenderWorker();
    RenderWorker(const RenderWorker &) = delete;
    RenderWorker &operator=(const RenderWorker &) = delete;

    // Main thread. Returns the request's generation.
    uint64_t request(const TitleDocument &doc, double titleFrame, std::map<std::string, std::string> fields, int width,
                     int height);

  private:
    struct Job
    {
        TitleDocument doc;
        double titleFrame = 0.0;
        std::map<std::string, std::string> fields;
        int width = 0, height = 0;
        uint64_t generation = 0;
    };
    // Shared with posted results, so a result arriving after the worker is
    // gone is dropped instead of calling into a dead canvas.
    struct Link
    {
        std::mutex mutex;
        Done done;
    };

    void run();

    std::shared_ptr<Link> m_link;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::optional<Job> m_pending;
    bool m_stop = false;
    uint64_t m_generation = 0;
    std::thread m_thread;
};

} // namespace ustudio::titles::app
