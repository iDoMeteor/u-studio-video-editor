#pragma once

#include "core/model/frame_time.h"
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

// Computes and caches per-clip audio waveform peak data (one normalized
// [0,1] peak value per frame across a clip's [in,out] range) as jobs on the
// app's worker pool, so drawing the timeline never blocks on decoding.
//
// A narrow, separate MLT touchpoint from EngineSync/PlaybackController —
// kept outside them specifically so waveform computation (which opens its
// own throwaway Producer/Profile per clip and can take a noticeable moment
// for long clips) never contends with the live tractor or consumer, and
// never blocks playback/editing while it works.
class WaveformCache
{
  public:
    // onReady is invoked (already marshaled onto the GLib main thread)
    // each time a previously-unavailable waveform finishes computing —
    // callers should just queue a redraw and call peaksFor() again.
    // Runs as jobs on `pool` (doc 19 MT3), at most `maxJobs` at once, one
    // per (resource, range); Interactive, since the timeline asks only for
    // clips it's drawing.
    WaveformCache(core::concurrency::ThreadPool &pool, std::function<void()> onReady, size_t maxJobs);
    // Calls shutdown().
    ~WaveformCache();
    // Stops taking work, cancels queued jobs, waits for running ones. Call
    // before the pool is destroyed. Main thread. Idempotent.
    void shutdown();
    // Most jobs that ran at the same time so far (for tests).
    size_t peakConcurrentJobs() const;

    WaveformCache(const WaveformCache &) = delete;
    WaveformCache &operator=(const WaveformCache &) = delete;

    // Returns the cached peaks for this exact (resource, in, out, fps), or
    // nullptr if not yet computed — which also kicks off a background
    // computation if one isn't already in flight for this key. The
    // returned pointer stays valid for the lifetime of this cache
    // (std::map never invalidates other entries' references on insert, and
    // an entry, once inserted, is never mutated again).
    //
    // fps must be the SEQUENCE's frame rate (core::Model::sequence().
    // profile.fps), the same rate `in`/`out` are already expressed in —
    // never a hardcoded stock rate. Verified empirically (audit E5): an
    // Mlt::Producer's own frame numbering (get_length(), seek()) is
    // normalized to whatever Mlt::Profile it was opened with, not the
    // source file's native rate -- the same file opened at 25fps vs.
    // 50fps reported get_length() of 928 vs. 1857, roughly double, not the
    // same number. Opening the job's throwaway Producer against a profile
    // whose fps doesn't match the sequence's would seek every job to the
    // wrong wall-clock position and silently draw a shifted, wrong-length
    // waveform for any project that isn't exactly that rate. fps is folded
    // into the cache key so a later project (a different sequence, a
    // different rate) never reuses another rate's peaks for what would
    // otherwise look like the same (resource, in, out).
    const std::vector<float> *peaksFor(const std::string &resource, int in, int out, core::Rational fps);

  private:
    struct Job
    {
        std::string key;
        std::string resource;
        int in;
        int out;
        core::Rational fps;
    };

    void schedule(); // m_mutex held
    std::vector<float> compute(const Job &job);
    static std::string keyFor(const std::string &resource, int in, int out, core::Rational fps);

    mutable std::mutex m_mutex;
    std::map<std::string, std::vector<float>> m_cache;
    std::set<std::string> m_inFlight;
    std::deque<Job> m_queue;
    core::concurrency::ThreadPool &m_pool;
    const size_t m_maxJobs;
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
