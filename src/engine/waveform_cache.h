#pragma once

#include "core/model/frame_time.h"

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

namespace core = ustudio::core;

// Computes and caches per-clip audio waveform peak data (one normalized
// [0,1] peak value per frame across a clip's [in,out] range) on a
// dedicated background worker thread, so drawing the timeline never blocks
// on decoding.
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
    explicit WaveformCache(std::function<void()> onReady);
    ~WaveformCache();

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

    void workerMain();
    static std::string keyFor(const std::string &resource, int in, int out, core::Rational fps);

    mutable std::mutex m_mutex;
    std::map<std::string, std::vector<float>> m_cache;
    std::set<std::string> m_inFlight;
    std::deque<Job> m_queue;
    std::condition_variable m_cv;
    std::thread m_worker;
    std::atomic<bool> m_quit{false};
    std::function<void()> m_onReady;
};

} // namespace ustudio::engine
