#include "mlt_engine.h"

#include "core/log.h"

#include <mlt++/Mlt.h>

#include <pulse/error.h>
#include <pulse/simple.h>

#include <algorithm>
#include <chrono>

namespace ustudio::engine {

namespace Log = ustudio::core::Log;

namespace {
constexpr int kAudioRate = 48000;
constexpr int kAudioChannels = 2;
} // namespace

MltEngine::MltEngine()
{
    // Mlt::Factory::init()/close() are NOT called here — FactoryPolicy owns
    // them exclusively, once, for the process lifetime. No tractor either:
    // this class has nothing to play until setTractor() is called (the app
    // does so once EngineSync has built one from the model).

    pa_sample_spec spec;
    spec.format = PA_SAMPLE_S16LE;
    spec.rate = kAudioRate;
    spec.channels = kAudioChannels;

    // A blocking pa_simple_write() only paces us to real time once the
    // server's buffer is full enough to push back. With the default (no
    // explicit attr), that buffer can be large enough that the first
    // stretch of playback decodes/writes far ahead of real time before
    // backpressure kicks in, which plays back like the whole clip is
    // running fast. Pin the buffer to ~150ms so backpressure starts
    // immediately.
    pa_buffer_attr attr;
    size_t targetBytes = pa_usec_to_bytes(150000, &spec);
    attr.maxlength = static_cast<uint32_t>(targetBytes);
    attr.tlength = static_cast<uint32_t>(targetBytes);
    // NOT 0: per <pulse/def.h>, prebuf=0 means "manual start/stop control" —
    // playback then never starts on its own, since only pa_stream_cork()
    // would trigger it, which the blocking simple API never calls. -1 is
    // the documented "same as tlength" default: normal auto-start once
    // ~150ms is buffered.
    attr.prebuf = static_cast<uint32_t>(-1);
    attr.minreq = static_cast<uint32_t>(targetBytes / 4);
    attr.fragsize = static_cast<uint32_t>(-1);

    int paError = 0;
    m_audioStream = pa_simple_new(nullptr, "u Studio Video Editor", PA_STREAM_PLAYBACK, nullptr, "preview", &spec,
                                  nullptr, &attr, &paError);
    if (!m_audioStream) {
        Log::warn(std::string("Audio output unavailable (") + pa_strerror(paError) + "); preview will be silent.");
    } else {
        Log::info("Audio output opened (48kHz stereo S16LE, ~150ms buffer)");
    }

    m_worker = std::thread([this] { pullLoopMain(); });
    Log::debug("MltEngine constructed, worker thread started");
}

void MltEngine::shutdown()
{
    m_quit.store(true);
    if (m_worker.joinable())
        m_worker.join();

    if (m_audioStream) {
        pa_simple_free(m_audioStream);
        m_audioStream = nullptr;
    }

    m_tractor.reset();
}

MltEngine::~MltEngine()
{
    Log::debug("MltEngine shutting down");
    shutdown();
}

void MltEngine::setTractor(std::shared_ptr<Mlt::Tractor> tractor)
{
    std::lock_guard<std::mutex> lock(m_mltMutex);

    int preservedPosition = m_tractor ? m_tractor->position() : 0;
    m_tractor = std::move(tractor);

    if (m_tractor) {
        int length = m_tractor->get_length();
        m_totalFramesCache.store(length);
        m_fpsCache.store(m_tractor->get_fps());
        int clamped = std::clamp(preservedPosition, 0, std::max(length - 1, 0));
        m_tractor->seek(clamped);
        m_lastKnownFrame.store(clamped);
    } else {
        m_totalFramesCache.store(0);
    }
}

void MltEngine::play()
{
    Log::debug("play() at frame " + std::to_string(currentFrame()));
    m_playing.store(true);
}

void MltEngine::pause()
{
    Log::debug("pause() at frame " + std::to_string(currentFrame()));
    m_playing.store(false);
}

void MltEngine::togglePlay()
{
    if (m_playing.load())
        pause();
    else
        play();
}

void MltEngine::seek(int frame)
{
    int length = totalFrames();
    if (frame < 0)
        frame = 0;
    if (length > 0 && frame >= length)
        frame = length - 1;
    m_seekRequest.store(frame);
}

int MltEngine::totalFrames() const
{
    return m_totalFramesCache.load();
}

double MltEngine::fps() const
{
    return m_fpsCache.load();
}

void MltEngine::setFrameCallback(FrameCallback cb)
{
    m_callback = std::move(cb);
}

void MltEngine::pullLoopMain()
{
    using namespace std::chrono;

    // Wall-clock playback schedule. A blocking pa_simple_write() looked like
    // a natural pacing signal, but it isn't a safe one: the moment the
    // server's buffer underruns (even briefly, e.g. audible as a click), the
    // very next write is accepted instantly instead of blocking, and the
    // loop races ahead pulling frames until the buffer re-fills — which
    // sounds exactly like alternating fast bursts and static. Anchoring to
    // a wall-clock schedule instead is immune to that feedback loop: audio
    // writes still happen, but they no longer decide our timing.
    bool wasPlaying = false;
    steady_clock::time_point playStartWallClock;
    int playStartFrame = 0;

    while (!m_quit.load()) {
        int seekTo = m_seekRequest.exchange(-1);
        bool playing = m_playing.load();

        bool hasTractor;
        {
            std::lock_guard<std::mutex> lock(m_mltMutex);
            hasTractor = m_tractor != nullptr;
        }

        if (!hasTractor || (seekTo < 0 && !playing)) {
            wasPlaying = false;
            std::this_thread::sleep_for(milliseconds(30));
            continue;
        }

        // Re-anchor on any resume from idle AND on any seek while already
        // playing (a scrub during playback). Without the seekTo>=0 half of
        // this, a mid-playback scrub leaves the schedule anchored to
        // wherever playback originally started: elapsedFrames vs. real
        // wall-clock time then diverges by tens of seconds, "behind
        // schedule" never recovers, and the loop stops sleeping entirely
        // for the rest of the session — pegging a core and flooding
        // g_idle_add() fast enough to make the whole app unresponsive.
        // Confirmed via debug logs: frame numbers jumping around (scrubs)
        // immediately followed by "behind schedule" climbing without bound.
        if (playing && (!wasPlaying || seekTo >= 0)) {
            std::lock_guard<std::mutex> lock(m_mltMutex);
            playStartWallClock = steady_clock::now();
            playStartFrame = seekTo >= 0 ? seekTo : m_tractor->position();
        }
        wasPlaying = playing;

        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;
        int frameNumber = 0;
        std::vector<uint8_t> audioPcm;

        {
            std::lock_guard<std::mutex> lock(m_mltMutex);
            if (!m_tractor)
                continue;

            if (seekTo >= 0)
                m_tractor->seek(seekTo);

            std::unique_ptr<Mlt::Frame> frame(m_tractor->get_frame());
            if (frame && frame->is_valid()) {
                mlt_image_format iFormat = mlt_image_rgba;
                int fw = 0;
                int fh = 0;
                uint8_t *image = frame->get_image(iFormat, fw, fh);
                if (image && fw > 0 && fh > 0) {
                    rgba.assign(image, image + static_cast<size_t>(fw) * fh * 4);
                    width = fw;
                    height = fh;
                }
                frameNumber = frame->get_position();

                // Only pull audio while actually playing — not on a
                // scrub-while-paused seek, which would otherwise play a
                // burst of stale audio for every frame dragged past.
                if (playing && m_audioStream) {
                    mlt_audio_format aFormat = mlt_audio_s16;
                    int frequency = kAudioRate;
                    int channels = kAudioChannels;
                    int samples = mlt_audio_calculate_frame_samples(static_cast<float>(m_tractor->get_fps()), frequency,
                                                                    frameNumber);
                    void *audio = frame->get_audio(aFormat, frequency, channels, samples);
                    if (audio && samples > 0) {
                        size_t bytes = static_cast<size_t>(samples) * channels * sizeof(int16_t);
                        auto *bytesPtr = static_cast<uint8_t *>(audio);
                        audioPcm.assign(bytesPtr, bytesPtr + bytes);
                    }
                }
            }

            // Deliberately no seek(position + 1) here for the normal
            // playing-forward case: get_frame() already auto-advances the
            // producer's internal position for the next sequential call.
            // Forcing an explicit seek before every frame instead makes the
            // producer treat each step as a random-access seek rather than
            // a cheap sequential decode — on a real file with long GOPs/
            // B-frames this desyncs audio badly (verified empirically: it
            // cut a real 88s clip's actual audio content down to the first
            // ~43s, silence after). seek() is still used above, but only
            // when there's an explicit seek request (scrub/jump).
            if (playing && frameNumber + 1 >= m_tractor->get_length())
                m_playing.store(false);
        }

        m_lastKnownFrame.store(frameNumber);

        if (!rgba.empty()) {
            auto *pending = new PendingFrame{this, std::move(rgba), width, height, frameNumber};
            g_idle_add(&MltEngine::deliverOnMainThread, pending);
        }

        // Write audio (still blocking — that's fine, it just contributes to
        // this iteration's real elapsed time now rather than being trusted
        // as the clock), then correct against the wall-clock schedule.
        if (playing && !audioPcm.empty() && m_audioStream) {
            int paError = 0;
            pa_simple_write(m_audioStream, audioPcm.data(), audioPcm.size(), &paError);
        }

        if (playing) {
            int elapsedFrames = frameNumber - playStartFrame + 1;
            double frameDurationSec = 1.0 / fps();
            auto target = playStartWallClock +
                          duration_cast<steady_clock::duration>(duration<double>(elapsedFrames * frameDurationSec));
            auto now = steady_clock::now();
            if (now < target) {
                std::this_thread::sleep_for(target - now);
            } else {
                // Behind schedule — don't sleep (catching up without a
                // corrective delay is what avoids a runaway-fast feedback
                // loop), but log it: a large or growing lag here is exactly
                // the signal that decode/seek can't keep up with real time
                // for a given file.
                auto behindMs = duration_cast<milliseconds>(now - target).count();
                if (behindMs > 100) {
                    Log::debug("[engine] Playback behind schedule by " + std::to_string(behindMs) + "ms at frame " +
                               std::to_string(frameNumber));
                }
            }
        }
    }
}

gboolean MltEngine::deliverOnMainThread(gpointer data)
{
    std::unique_ptr<PendingFrame> pending(static_cast<PendingFrame *>(data));
    if (pending->engine->m_callback) {
        pending->engine->m_callback(std::move(pending->rgba), pending->width, pending->height, pending->frameNumber);
    }
    return G_SOURCE_REMOVE;
}

} // namespace ustudio::engine
