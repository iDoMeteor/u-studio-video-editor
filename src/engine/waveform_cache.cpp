#include "waveform_cache.h"

#include "core/log.h"

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

            // Cap decoded samples regardless of clip length: drawWaveform()
            // (app_window.cpp) already re-buckets whatever ends up in
            // `peaks` down to the clip's on-screen pixel width, so one peak
            // per VIDEO FRAME on a long clip is far more resolution than
            // anything ever displays -- measured 18.2 SECONDS for a single
            // ~62-minute (110,854-frame) real clip's waveform job, entirely
            // spent in per-frame Mlt::Producer::get_frame() calls, and
            // reported as the live playback consumer effectively starved
            // for the whole time this ran on its own background thread
            // decoding the very same file. Above kMaxPeaks frames, stride
            // through the clip instead of decoding every one of them --
            // needs an explicit seek() per sample once striding, unlike the
            // sequential (stride == 1) case below it, where get_frame()
            // auto-advancing on its own is enough (same lesson as the
            // playback engine: no per-frame seek() needed there either).
            constexpr int kMaxPeaks = 2000;
            int stride = frameCount > kMaxPeaks ? (frameCount + kMaxPeaks - 1) / kMaxPeaks : 1;
            size_t peakCapacity = frameCount > 0 ? static_cast<size_t>((frameCount - 1) / stride) + 1 : 0;
            peaks.reserve(peakCapacity);

            for (int i = 0; i < frameCount; i += stride) {
                if (stride > 1)
                    producer.seek(job.in + i);
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

        // Through MainThreadDispatcher rather than a raw g_idle_add(): one
        // hand-off mechanism for every worker-to-main-thread post, with a
        // lifetime guard, and the one place the TSan annotations for that
        // hand-off live (sanitizer report S5).
        MainThreadDispatcher::post(m_lifetime, m_onReady);
    }
}

} // namespace ustudio::engine
