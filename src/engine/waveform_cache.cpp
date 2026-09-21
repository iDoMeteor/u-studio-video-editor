#include "waveform_cache.h"

#include "core/log.h"

#include <glib.h>
#include <mlt++/Mlt.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ustudio::engine {

namespace Log = ustudio::core::Log;

WaveformCache::WaveformCache(std::function<void()> onReady) : m_onReady(std::move(onReady))
{
    m_worker = std::thread([this] { workerMain(); });
}

WaveformCache::~WaveformCache()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_quit.store(true);
    }
    m_cv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

std::string WaveformCache::keyFor(const std::string &resource, int in, int out, core::Rational fps)
{
    return resource + "|" + std::to_string(in) + "|" + std::to_string(out) + "|" + std::to_string(fps.num) + "/" +
           std::to_string(fps.den);
}

const std::vector<float> *WaveformCache::peaksFor(const std::string &resource, int in, int out, core::Rational fps)
{
    std::string key = keyFor(resource, in, out, fps);
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_cache.find(key);
    if (it != m_cache.end())
        return &it->second;

    if (m_inFlight.find(key) == m_inFlight.end()) {
        m_inFlight.insert(key);
        m_queue.push_back(Job{key, resource, in, out, fps});
        m_cv.notify_one();
    }
    return nullptr;
}

void WaveformCache::workerMain()
{
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_quit.load() || !m_queue.empty(); });
            if (m_queue.empty()) {
                if (m_quit.load())
                    return;
                continue;
            }
            job = m_queue.front();
            m_queue.pop_front();
        }

        std::vector<float> peaks;
        Log::ScopedTimer timer("[waveform] job " + job.resource + " [" + std::to_string(job.in) + "," +
                               std::to_string(job.out) + "]");

        // Independent Profile/Producer per job — never touches MltEngine's
        // own objects, m_mltMutex, or the live playback/editing state.
        // frame_rate is set from the job's (sequence) fps, not a hardcoded
        // stock profile: see peaksFor()'s comment for why a mismatched
        // rate here would seek every job to the wrong wall-clock position.
        Mlt::Profile profile;
        profile.set_frame_rate(job.fps.num, job.fps.den);
        Mlt::Producer producer(profile, job.resource.c_str());
        if (producer.is_valid()) {
            producer.seek(job.in);
            int frameCount = std::max(job.out - job.in + 1, 0);
            peaks.reserve(static_cast<size_t>(frameCount));

            // No per-frame seek() here either — same lesson as the
            // playback engine: get_frame() already auto-advances the
            // producer's position for the next call.
            for (int i = 0; i < frameCount; ++i) {
                std::unique_ptr<Mlt::Frame> frame(producer.get_frame());
                float peak = 0.0f;
                if (frame && frame->is_valid()) {
                    mlt_audio_format aFormat = mlt_audio_s16;
                    int frequency = 48000;
                    int channels = 2;
                    int samples =
                        mlt_audio_calculate_frame_samples(static_cast<float>(profile.fps()), frequency, job.in + i);
                    void *audio = frame->get_audio(aFormat, frequency, channels, samples);
                    if (audio && samples > 0) {
                        auto *s16 = static_cast<int16_t *>(audio);
                        int peakAbs = 0;
                        int n = samples * channels;
                        for (int s = 0; s < n; ++s)
                            peakAbs = std::max(peakAbs, std::abs(static_cast<int>(s16[s])));
                        peak = static_cast<float>(peakAbs) / 32767.0f;
                    }
                }
                peaks.push_back(peak);
            }
        } else {
            Log::warn("[waveform] could not open " + job.resource);
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cache.emplace(job.key, std::move(peaks));
            m_inFlight.erase(job.key);
        }

        // AppWindow (the usual onReady target) is never destroyed during
        // normal operation — see main.cpp's "leaked intentionally" note —
        // so capturing/calling back into it from here is safe for the
        // process's lifetime.
        auto *cb = new std::function<void()>(m_onReady);
        g_idle_add(
            [](gpointer data) -> gboolean {
                std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()> *>(data));
                (*fn)();
                return G_SOURCE_REMOVE;
            },
            cb);
    }
}

} // namespace ustudio::engine
