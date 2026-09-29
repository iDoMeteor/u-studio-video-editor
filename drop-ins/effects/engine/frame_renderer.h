#pragma once

// One frame of a clip through a stack of effects, rendered off the live
// graph (doc 15, "Effect Browser": tiles and live audition). A worker
// thread of its own opens throwaway producers (engine::openProducer, Worker:
// the CPU chain whatever the GPU state, ADR-019), never the live tractor, so
// the preview and playback are never rebuilt or stalled. Results arrive on
// the GLib main loop; the newest request of each kind wins.
//
// Only effects the health scan found ok may be rendered here: this runs in
// the editor's process (app/browser.cpp checks before asking).

#include "core/model/types.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ustudio::effects {

struct FrameRequest
{
    std::string resource;             // the clip's media, as the loader opens it
    core::Profile profile;            // the sequence's
    core::FrameIndex sourceFrame = 0; // in the media, at the sequence's rate
    core::FrameIndex clipIn = 0;      // the clip's cut, so keyframes count from its start
    core::FrameIndex clipOut = 0;
    std::vector<core::Effect> effects; // in order (the stack, plus the one auditioned)
    int width = 160;
    int height = 90;

    // Identity for the cache: everything above.
    std::string key() const;
};

struct RenderedFrame
{
    std::vector<uint8_t> rgba; // width * height * 4, straight alpha
    int width = 0;
    int height = 0;
};

class FrameRenderer
{
  public:
    using Done = std::function<void(const RenderedFrame &frame)>;
    // The worker closes its media after this long without a request: an
    // open decoder is the renderer's largest cost (frame_renderer.cpp).
    static constexpr std::chrono::milliseconds kReleaseIdleMedia{2000};

    explicit FrameRenderer(size_t cacheEntries = 400);
    ~FrameRenderer(); // stop()
    // Stops the worker, waiting for the frame in hand; its producers are
    // closed on it. Must happen before Mlt::Factory::close() (CLAUDE.md:
    // nothing MLT outlives the factory): the Browser calls it when its
    // window goes. Later requests are dropped. Idempotent.
    void stop();
    // stop() for every renderer alive (main thread): the drop-in calls it
    // when the application shuts down, before main() closes the factory.
    // Renderers owned by statics are destroyed only after that, at exit(),
    // when closing a producer would call into an unloaded module (a crash
    // at quit in the demo tour, 2026-09-29).
    static void stopAll();
    // Whether the worker has media open (for tests; any thread).
    bool holdsMedia() const
    {
        return m_holdsMedia.load();
    }
    FrameRenderer(const FrameRenderer &) = delete;
    FrameRenderer &operator=(const FrameRenderer &) = delete;

    // The cached frame, or null (main thread).
    const RenderedFrame *cached(const FrameRequest &request) const;
    // Renders `request`, then calls `done` on the main loop (at once, from
    // the cache, when it's there). Requests with a lower `generation` than
    // the newest seen for their `lane` are dropped unrendered: a new
    // search or a new audition makes the old ones moot. Lane 0 goes first.
    void request(FrameRequest request, int lane, uint64_t generation, Done done);

    // Tests: renders on the calling thread, no cache.
    static RenderedFrame renderNow(const FrameRequest &request);

  private:
    struct Job
    {
        FrameRequest request;
        int lane;
        uint64_t generation;
        Done done;
    };
    void run();
    void deliver(const std::string &key, RenderedFrame frame, Done done);

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Job> m_jobs;
    std::map<int, uint64_t> m_newest; // lane -> generation
    bool m_stopping = false;
    std::atomic<bool> m_holdsMedia{false};
    std::thread m_worker;

    // Main thread: an LRU of rendered frames.
    size_t m_capacity;
    std::list<std::string> m_order; // most recent first
    std::map<std::string, std::pair<RenderedFrame, std::list<std::string>::iterator>> m_cache;
    // Results reach the main loop through engine::MainThreadDispatcher,
    // dropped once this is reset (stop()).
    std::shared_ptr<void> m_token = std::make_shared<char>(0);
};

} // namespace ustudio::effects
