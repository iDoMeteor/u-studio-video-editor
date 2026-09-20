#include "playback_controller.h"

#include "core/log.h"

#include <algorithm>

namespace ustudio::engine {

namespace Log = ustudio::core::Log;

namespace {
// Tried in order (ADR-002); the first one whose Mlt::Consumer construction
// reports valid wins. `null` paces to the profile fps with no sound, so it
// always succeeds and playback stays usable even with no audio backend.
constexpr const char *kConsumerBackends[] = {"sdl2_audio", "rtaudio", "null"};
} // namespace

PlaybackController::PlaybackController() = default;

PlaybackController::~PlaybackController()
{
    shutdown();
}

void PlaybackController::shutdown()
{
    if (m_consumer)
        m_consumer->stop(); // joins the consumer's own thread(s) -- doc 05
    // Fence against a frame-show callback that was already in flight (past
    // the point where stop() could prevent it starting) when the line
    // above returned: handleFrameShow() holds this same mutex for its
    // whole body, so acquiring and releasing it here blocks until any such
    // call has finished, before anything it might touch gets destroyed.
    // Found empirically: without this, a 100-iteration setTractor()+play()
    // +shutdown() stress loop crashed with a heap corruption (double
    // free), reproducing reliably enough to be worth this fence rather
    // than trusting stop() alone.
    { std::lock_guard<std::mutex> lock(m_frameShowMutex); }
    m_frameShowEvent.reset();
    m_consumer.reset();
    m_consumerProfile = nullptr;
    m_tractor.reset();
}

bool PlaybackController::selectAndStartConsumer(Mlt::Tractor &tractor)
{
    Mlt::Profile *profile = tractor.profile();

    for (const char *name : kConsumerBackends) {
        auto consumer = std::make_unique<Mlt::Consumer>(*profile, name);
        if (!consumer->is_valid())
            continue;

        // Consumer configuration (doc 05's table). Properties a given
        // backend doesn't recognize are harmless no-ops in MLT's property
        // system, so this set is applied uniformly across all three.
        consumer->set("real_time", 1); // drop frames to keep real time, 1 decode thread
        consumer->set("mlt_image_format", "rgba"); // matches GDK_MEMORY_R8G8B8A8, no conversion
        consumer->set("channels", 2);
        consumer->set("frequency", 48000);
        consumer->set("buffer", 25);
        consumer->set("terminate_on_pause", 0); // stay alive across pause; we pause via speed 0
        consumer->set("volume", m_volume.load());

        if (consumer->connect(tractor) != 0)
            continue; // shouldn't happen for a freshly-valid consumer, but fall through defensively

        std::unique_ptr<Mlt::Event> event(
            consumer->listen("consumer-frame-show", this, &PlaybackController::frameShowTrampoline));
        if (!event || !event->is_valid())
            continue;

        if (consumer->start() != 0) {
            Log::warn(std::string("[engine] Consumer '") + name + "' connected but failed to start; trying the next backend");
            continue;
        }

        m_consumer = std::move(consumer);
        m_frameShowEvent = std::move(event);
        m_consumerProfile = profile;
        m_backendName = name;
        applyResolvedScale();

        Log::info(std::string("[engine] Playback consumer: ") + name);
        return true;
    }

    Log::error("[engine] No playback consumer could be started (not even 'null') -- playback disabled");
    return false;
}

void PlaybackController::setTractor(std::shared_ptr<Mlt::Tractor> tractor)
{
    if (!tractor) {
        shutdown();
        return;
    }

    int preservedPosition = m_playing.load() ? m_lastKnownFrame.load() : m_pausedPosition.load();
    bool wasPlaying = m_playing.load();
    double previousSpeed = m_speed.load();

    Mlt::Profile *newProfile = tractor->profile();

    if (m_consumer && newProfile == m_consumerProfile) {
        // Hot swap: the common case (every ordinary edit rebuilds the
        // tractor via EngineSync). Reconnect the already-running consumer
        // to the new tractor object in place -- no stop/restart, so
        // editing never clicks or drops the audio device.
        m_tractor = std::move(tractor);
        m_consumer->connect(*m_tractor);
    } else {
        // Either the very first tractor we've ever seen, or the profile
        // differs from what the running consumer was built for -- a
        // project load with a different resolution/fps (doc 05: "changing
        // the sequence profile rebuilds everything: stop consumer, destroy
        // tractor, rebuild, reconnect").
        shutdown();
        m_tractor = std::move(tractor);
        if (!selectAndStartConsumer(*m_tractor)) {
            m_tractor.reset();
            return;
        }
    }

    int total = m_tractor->get_length();
    int clamped = std::clamp(preservedPosition, 0, std::max(total - 1, 0));
    m_tractor->seek(clamped);
    m_pausedPosition.store(clamped);
    m_lastKnownFrame.store(clamped);

    if (wasPlaying)
        play(previousSpeed);
    else
        pause();
}

void PlaybackController::setFrameCallback(FrameCallback cb)
{
    m_callback = std::move(cb);
}

void PlaybackController::play(double speed)
{
    if (!m_tractor)
        return;
    m_tractor->set_speed(speed);
    m_speed.store(speed);
    m_playing.store(speed != 0.0);
    applyResolvedScale();
}

void PlaybackController::pause()
{
    if (!m_tractor)
        return;
    m_pausedPosition.store(m_lastKnownFrame.load());
    m_tractor->set_speed(0);
    m_speed.store(0.0);
    m_playing.store(false);
    if (m_consumer) {
        // The kdenlive pattern (doc 05): purge the prefetch buffer so the
        // frame that shows is the one at the playhead, then force exactly
        // one frame through. This is what gives frame-accurate pause.
        m_consumer->purge();
        m_consumer->set("refresh", 1);
    }
    applyResolvedScale();
}

void PlaybackController::togglePlay()
{
    if (isPlaying())
        pause();
    else
        play(1.0);
}

void PlaybackController::seek(int frame)
{
    if (!m_tractor)
        return;
    int total = totalFrames();
    frame = std::clamp(frame, 0, std::max(total - 1, 0));
    m_pausedPosition.store(frame);
    m_tractor->seek(frame);
    if (!m_playing.load() && m_consumer) {
        m_consumer->purge();
        m_consumer->set("refresh", 1);
    }
}

void PlaybackController::stepFrame(int delta)
{
    pause();
    seek(currentFrame() + delta);
}

void PlaybackController::toHome()
{
    seek(0);
}

void PlaybackController::toEnd()
{
    seek(std::max(totalFrames() - 1, 0));
}

void PlaybackController::setLoopRange(std::optional<std::pair<int, int>> range)
{
    if (range && range->first >= range->second)
        range.reset(); // degenerate/inverted range: treat as "no loop" rather than refusing silently later
    m_loopRange = range;
}

void PlaybackController::setVolume(double volume)
{
    m_volume.store(std::clamp(volume, 0.0, 1.0));
    applyVolumeToConsumer();
}

void PlaybackController::applyVolumeToConsumer()
{
    if (m_consumer)
        m_consumer->set("volume", m_volume.load());
}

void PlaybackController::setPreviewScale(PreviewScale scale)
{
    m_previewScalePreference = scale;
    applyResolvedScale();
}

void PlaybackController::applyResolvedScale()
{
    if (!m_consumer)
        return;

    double resolved = 1.0;
    switch (m_previewScalePreference) {
    case PreviewScale::Full:
        resolved = 1.0;
        break;
    case PreviewScale::Half:
        resolved = 0.5;
        break;
    case PreviewScale::Quarter:
        resolved = 0.25;
        break;
    case PreviewScale::Auto: {
        // doc 05: half-res while playing a 1080p+ profile (halves
        // decode/upload cost during playback); full when paused, so a
        // stopped-on frame is crisp.
        bool isHighRes = m_tractor && m_tractor->profile() && m_tractor->profile()->height() >= 1080;
        resolved = (m_playing.load() && isHighRes) ? 0.5 : 1.0;
        break;
    }
    }
    m_consumer->set("scale", resolved);
}

int PlaybackController::currentFrame() const
{
    return m_playing.load() ? m_lastKnownFrame.load() : m_pausedPosition.load();
}

int PlaybackController::totalFrames() const
{
    return m_tractor ? m_tractor->get_length() : 0;
}

double PlaybackController::fps() const
{
    return m_tractor ? m_tractor->get_fps() : 0.0;
}

void PlaybackController::LatestFrameSlot::store(FrameData data)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_data = std::move(data);
}

std::optional<PlaybackController::FrameData> PlaybackController::LatestFrameSlot::take()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_data)
        return std::nullopt;
    FrameData result = std::move(*m_data);
    m_data.reset();
    return result;
}

// ---- MLT consumer thread: keep this path minimal (copy + post), never touch GTK or non-atomic app state ----

void PlaybackController::frameShowTrampoline(mlt_properties /*owner*/, void *self, mlt_event_data data)
{
    static_cast<PlaybackController *>(self)->handleFrameShow(Mlt::EventData(data));
}

void PlaybackController::handleFrameShow(const Mlt::EventData &eventData)
{
    // Held for the whole body: shutdown()'s fence (see its comment)
    // acquires this same mutex after stop() to guarantee it never starts
    // destroying anything this function might still be touching.
    std::lock_guard<std::mutex> lock(m_frameShowMutex);

    Mlt::Frame frame(eventData.to_frame());
    if (!frame.is_valid())
        return;

    mlt_image_format format = mlt_image_rgba;
    int width = 0;
    int height = 0;
    uint8_t *image = frame.get_image(format, width, height);
    if (!image || width <= 0 || height <= 0)
        return;

    int position = frame.get_position();
    m_lastKnownFrame.store(position);

    FrameData data;
    data.rgba.assign(image, image + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    data.width = width;
    data.height = height;
    data.position = position;
    m_slot.store(std::move(data));

    bool expected = false;
    if (m_drainQueued.compare_exchange_strong(expected, true))
        MainThreadDispatcher::post(m_lifetimeToken, [this] { drainSlot(); });
}

void PlaybackController::drainSlot()
{
    m_drainQueued.store(false);
    std::optional<FrameData> data = m_slot.take();
    if (!data)
        return;

    if (m_loopRange && data->position >= m_loopRange->second) {
        // Not the generic seek(): while playing, seek() deliberately
        // leaves the prefetch buffer alone ("the consumer catches up",
        // doc 05) for a normal user scrub -- but that means frames already
        // queued past loop-out (up to a full `buffer` setting's worth, 25
        // by default) would still fire before the seek took effect,
        // running position well past loop-out instead of the one-frame
        // overshoot doc 05 expects. purge() here cuts that queue
        // immediately, same as the paused/scrub path already does.
        m_pausedPosition.store(m_loopRange->first);
        m_tractor->seek(m_loopRange->first);
        if (m_consumer)
            m_consumer->purge();
    }

    if (m_callback)
        m_callback(std::move(data->rgba), data->width, data->height, data->position);
}

} // namespace ustudio::engine
