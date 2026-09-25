#pragma once

#include "core/concurrency/thread_pool.h"
#include "dispatcher.h"

#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace ustudio::engine {

namespace core = ustudio::core;

// Computes and caches a small representative-frame thumbnail per media
// file (keyed by resource path), and the timeline's frame thumbnails, as
// jobs on the app's worker pool (doc 19 MT3). Each job opens its own
// throwaway Profile/Producer -- never the live tractor or consumer -- so it
// never contends with playback or editing.
//
// Pending requests are grouped by file (and rate): one job opens the
// producer once and walks that file's pending frames, newest first, since a
// strip asks for many frames of one file and opening costs far more than
// decoding one frame. At most `maxJobs` run at once, so imports and probes
// on the same pool still get threads.
//
// A thumbnail isn't frame-accurate against any sequence's fps -- it's just
// "a representative picture of this file" for a media-browser row -- so
// that key is the resource path alone.
class ThumbnailCache
{
  public:
    struct Data
    {
        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;
    };

    // onReady is invoked (already marshaled onto the GLib main thread)
    // each time a previously-unavailable thumbnail finishes computing --
    // callers should just rebuild whatever row was showing a placeholder
    // and call thumbnailFor() again.
    // `priority`: Interactive for the timeline's strips (on screen as they
    // are asked for), Background for the media browser (its list asks for
    // every row, shown or not).
    // `width`: the thumbnails' width in pixels (height from the source's
    // shape); `maxFrameThumbnails`: how many frame thumbnails are kept.
    ThumbnailCache(core::concurrency::ThreadPool &pool, std::function<void()> onReady, size_t maxJobs,
                   core::concurrency::Priority priority, int width = kDefaultWidth,
                   size_t maxFrameThumbnails = kMaxFrameThumbnails);
    // Calls shutdown().
    ~ThumbnailCache();

    // Stops taking work, cancels queued jobs, waits for running ones. Call
    // before the pool is destroyed; later requests return nullptr and
    // schedule nothing. Main thread. Idempotent.
    void shutdown();

    ThumbnailCache(const ThumbnailCache &) = delete;
    ThumbnailCache &operator=(const ThumbnailCache &) = delete;

    // Returns the cached thumbnail for `resource`, or nullptr if not yet
    // computed -- which also kicks off a background computation if one
    // isn't already in flight for this resource. The returned pointer
    // stays valid for the lifetime of this cache (std::map never
    // invalidates other entries' references on insert, and an entry,
    // once inserted, is never mutated again).
    const Data *thumbnailFor(const std::string &resource);

    // The timeline's thumbnail strips (doc 06): the picture at `frame` of
    // `resource`, counted at `fpsNum/fpsDen` (the sequence's rate, which
    // clip in/out points use). Null until computed, like thumbnailFor(),
    // and newest requests are served first, so what's on screen now comes
    // before what was scrolled past. At most kMaxFrameThumbnails are kept;
    // the oldest go first, so a returned pointer is only valid until the
    // next call to this function (main thread only).
    const Data *frameThumbnail(const std::string &resource, int frame, int fpsNum, int fpsDen);
    // Starts a new round of frame requests (one timeline snapshot). A queued
    // frame job that isn't requested again in the latest round is dropped
    // before decoding: the view has moved on (post-M3 audit P5: 24 zoom
    // steps queued 102 decodes, none ever cancelled).
    void newFrameGeneration();
    // Frame thumbnails decoded so far (for tests).
    size_t frameThumbnailsDecoded() const;
    // Most jobs that ran at the same time so far (for tests).
    size_t peakConcurrentJobs() const;
    static constexpr size_t kMaxFrameThumbnails = 800;
    // A media-browser row icon or a timeline strip, not a preview -- kept
    // small so dozens of imports don't hold full-resolution frames. Height
    // follows the source's own aspect ratio; callers letterbox.
    static constexpr int kDefaultWidth = 120;

  private:
    struct Job
    {
        std::string resource;
        int frame = -1; // -1: a representative frame, for thumbnailFor()
        int fpsNum = 0, fpsDen = 1;
        std::string key;
    };
    // One file at one rate: what a pool job opens once.
    struct Batch
    {
        std::deque<Job> pending; // newest frame requests first
        bool running = false;
    };

    void enqueue(Job job, bool newest); // m_mutex held
    void schedule();                    // m_mutex held
    void runBatch(const std::string &batchKey, std::stop_token stop);
    // A frame job the latest round didn't ask for again (P5). m_mutex held.
    bool isStale(const Job &job);

    core::concurrency::ThreadPool &m_pool;
    const size_t m_maxJobs;
    const core::concurrency::Priority m_priority;
    const int m_width;
    const size_t m_maxFrameThumbnails;
    mutable std::mutex m_mutex;
    std::map<std::string, Data> m_cache;
    std::deque<std::string> m_frameOrder; // frame-thumbnail keys, oldest first
    uint64_t m_generation = 0;
    std::map<std::string, uint64_t> m_requestedIn; // queued frame key -> latest round that asked for it
    size_t m_framesDecoded = 0;
    std::set<std::string> m_inFlight;
    std::map<std::string, Batch> m_batches;
    std::deque<std::string> m_readyBatches; // batch keys with pending work and no job, newest first
    size_t m_running = 0;
    size_t m_peakRunning = 0;
    bool m_quit = false;
    std::vector<core::concurrency::JobHandle> m_jobs;
    std::function<void()> m_onReady;
    // Guards the ready callbacks posted through MainThreadDispatcher: if
    // this cache is destroyed first, a still-pending callback is dropped
    // instead of calling a copied m_onReady whose captures may be gone.
    // Declared last, so it's destroyed first, after the destructor has
    // waited for every job.
    MainThreadDispatcher::LifetimeToken m_lifetime = MainThreadDispatcher::makeToken();
};

} // namespace ustudio::engine
