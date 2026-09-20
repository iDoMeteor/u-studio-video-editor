#pragma once

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

    // Returns the cached peaks for this exact (resource, in, out), or
    // nullptr if not yet computed — which also kicks off a background
    // computation if one isn't already in flight for this key. The
    // returned pointer stays valid for the lifetime of this cache
    // (std::map never invalidates other entries' references on insert, and
    // an entry, once inserted, is never mutated again).
    const std::vector<float> *peaksFor(const std::string &resource, int in, int out);

  private:
    struct Job
    {
        std::string key;
        std::string resource;
        int in;
        int out;
    };

    void workerMain();
    static std::string keyFor(const std::string &resource, int in, int out);

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
