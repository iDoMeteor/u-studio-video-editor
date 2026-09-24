#pragma once

#include "dispatcher.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace ustudio::engine {

// Computes and caches a small representative-frame thumbnail per media
// file (keyed by resource path) on a dedicated background worker thread --
// the same architecture as WaveformCache (see its own class comment for
// why this lives outside EngineSync/PlaybackController: opening a
// throwaway Producer per job must never contend with the live tractor or
// consumer, and must never block playback/editing while it works).
//
// Unlike WaveformCache, a thumbnail doesn't need to be frame-accurate
// against any particular sequence's fps -- it's just "a representative
// picture of this file" for a media-browser row -- so the cache key is
// the resource path alone, and the worker opens its own default Profile
// rather than matching any sequence's rate.
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
    explicit ThumbnailCache(std::function<void()> onReady);
    ~ThumbnailCache();

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
    static constexpr size_t kMaxFrameThumbnails = 800;

  private:
    struct Job
    {
        std::string resource;
        int frame = -1; // -1: a representative frame, for thumbnailFor()
        int fpsNum = 0, fpsDen = 1;
        std::string key;
    };

    void workerMain();

    mutable std::mutex m_mutex;
    std::map<std::string, Data> m_cache;
    std::deque<std::string> m_frameOrder; // frame-thumbnail keys, oldest first
    std::set<std::string> m_inFlight;
    std::deque<Job> m_queue;
    std::condition_variable m_cv;
    std::thread m_worker;
    std::atomic<bool> m_quit{false};
    std::function<void()> m_onReady;
    // Guards the ready callbacks posted through MainThreadDispatcher: if
    // this cache is destroyed first, a still-pending callback is dropped
    // instead of calling a copied m_onReady whose captures may be gone.
    // Declared last, so it's destroyed first, after the destructor has
    // already joined the worker.
    MainThreadDispatcher::LifetimeToken m_lifetime = MainThreadDispatcher::makeToken();
};

} // namespace ustudio::engine
