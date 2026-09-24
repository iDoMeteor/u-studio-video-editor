#include "playback_controller.h"

#include "core/log.h"

#include <algorithm>

namespace ustudio::engine {

namespace Log = ustudio::core::Log;

namespace {
// Tried in order (ADR-002); the first one whose Mlt::Consumer construction
// reports valid wins. `null` always succeeds, but it does not pace: in a
// device-less container it showed 9,000+ frames a second (2026-09-24). And
// with no audio device rtaudio's start() returns 0 and then never shows a
// frame, so it is picked and playback stalls. SDL_AUDIODRIVER=dummy is the
// working setup there (tests/engine/meson.build).
constexpr const char *kConsumerBackends[] = {"sdl2_audio", "rtaudio", "null"};
} // namespace

PlaybackController::PlaybackController() = default;

PlaybackController::~PlaybackController()
{
    shutdown();
}

void PlaybackController::shutdown()
{
    if (m_consumer) {
        Log::debug(std::string("[engine] shutdown(): stopping consumer '") + m_backendName + "'");
        m_consumer->stop(); // joins the consumer's own thread(s) -- doc 05
    } else {
        Log::debug("[engine] shutdown(): no consumer to stop");
    }
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
    m_tractor.reset();
}

bool PlaybackController::selectAndStartConsumer(Mlt::Tractor &tractor)
{
    Log::ScopedTimer timer("[engine] selectAndStartConsumer");
    // Mlt::Service::profile() heap-allocates a fresh wrapper around a
    // *clone* of the service's profile on every call (confirmed
    // empirically alongside the E2 fix: two calls on the same tractor
    // gave different wrapper addresses) -- ours to free, hence the
    // unique_ptr rather than a raw pointer this function used to leak.
    std::unique_ptr<Mlt::Profile> profile(tractor.profile());

    for (const char *name : kConsumerBackends) {
        auto consumer = std::make_unique<Mlt::Consumer>(*profile, name);
        if (!consumer->is_valid()) {
            Log::debug(std::string("[engine] Consumer '") + name + "' not valid on this machine, trying the next backend");
            continue;
        }

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

        if (consumer->connect(tractor) != 0) {
            Log::warn(std::string("[engine] Consumer '") + name +
                      "' connect() failed unexpectedly for a freshly-valid consumer; trying the next backend");
            continue;
        }

        std::unique_ptr<Mlt::Event> event(
            consumer->listen("consumer-frame-show", this, &PlaybackController::frameShowTrampoline));
        if (!event || !event->is_valid()) {
            Log::warn(std::string("[engine] Consumer '") + name +
                      "' could not listen for consumer-frame-show; trying the next backend");
            continue;
        }

        if (consumer->start() != 0) {
            Log::warn(std::string("[engine] Consumer '") + name + "' connected but failed to start; trying the next backend");
            continue;
        }

        m_consumer = std::move(consumer);
        m_frameShowEvent = std::move(event);
        m_backendName = name;
        ++m_consumerRestartCount;
        applyResolvedScale();

        Log::info(std::string("[engine] Playback consumer: ") + name + " (restart #" +
                  std::to_string(m_consumerRestartCount) + ")");
        return true;
    }

    Log::error("[engine] No playback consumer could be started (not even 'null') -- playback disabled");
    return false;
}

void PlaybackController::setTractor(std::shared_ptr<Mlt::Tractor> tractor)
{
    Log::ScopedTimer timer("[engine] setTractor");
    if (!tractor) {
        Log::debug("[engine] setTractor(nullptr): shutting down");
        shutdown();
        return;
    }

    int preservedPosition = m_playing.load() ? m_lastKnownFrame.load() : m_pausedPosition.load();
    bool wasPlaying = m_playing.load();
    double previousSpeed = m_speed.load();
    Log::debug("[engine] setTractor: wasPlaying=" + std::to_string(wasPlaying) +
               " preservedPosition=" + std::to_string(preservedPosition));

    // Always a full stop/reselect/restart -- never Mlt::Consumer::connect()
    // on an already-running consumer. An earlier version of this function
    // tried exactly that as an optimisation for the common case (every
    // ordinary edit rebuilds the tractor via EngineSync, same profile), to
    // avoid closing and reopening the real audio device on every edit.
    // Confirmed via a standalone repro AND a doctest regression test that
    // it corrupts MLT's internal state: connecting a live consumer to a
    // new tractor while its background read-ahead (prefetch) thread is
    // still running on the old one crashes later, inside MLT's own
    // consumer_read_ahead_thread/mlt_service_get_frame, reproducing 3/3
    // runs with a GDB backtrace pointing at freed memory from the tractor
    // that had just been swapped out. The full restart path below is the
    // one actually proven safe (100/100 clean runs in the
    // shutdown-during-playback stress test) -- paying for a device
    // close/reopen on every edit is the honest cost of that safety until
    // hot-swapping a live consumer's producer can be shown safe some other
    // way (CLAUDE.md: never destroy an MLT service a running consumer can
    // still reach -- reconnecting away from one while its own read-ahead
    // thread is mid-flight turns out to be exactly that).
    shutdown();
    m_tractor = std::move(tractor);
    if (!selectAndStartConsumer(*m_tractor)) {
        m_tractor.reset();
        return;
    }

    int total = m_tractor->get_length();
    int clamped = std::clamp(preservedPosition, 0, std::max(total - 1, 0));
    m_tractor->seek(clamped);
    m_pausedPosition.store(clamped);
    m_lastKnownFrame.store(clamped);

    // Full pause() after the fresh start, purge included. Sanitizer report
    // S4 suggested skipping the purge here (MLT's mlt_consumer_purge()
    // reads an unsynchronised `started` flag right after start(), a
    // theoretical ARM hazard), and 4e9c602 did that. It was reverted: the
    // purge also drops the frame the fresh consumer queued at position 0
    // before the seek above. Without it, that stale frame could reach the
    // screen after the preserved one. The full suite then failed "pause
    // shows the exact frame sought to" in 2 of 6 runs under load, against
    // 0 of 6 with the purge. The S4 race itself never reproduced under
    // TSan here.
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
    Log::debug("[engine] play(speed=" + std::to_string(speed) + ")");
    // Wakes the consumer thread. Found empirically (owner reports: "I can
    // still scrub but not play", then "it doesn't play from the first
    // frame after the first import either"): after pause() or a paused
    // seek(), sdl2_audio's consumer thread shows one frame and then blocks
    // on a condition variable that only a write to the "refresh" property
    // wakes (consumer_refresh_cb in MLT's consumer_sdl2_audio.c -- any
    // write fires it; the value written doesn't matter), so set_speed()
    // alone afterward never resumed continuous pulling. Reproduced 100% with a standalone repro against a real
    // file: a SINGLE seek() (e.g. one click on the timeline -- exactly
    // what both reports amount to, since setTractor() ends every ordinary
    // edit, including an import, with an implicit pause() too) followed by
    // play() froze at the seeked position indefinitely; confirmed fixed by
    // this line alone. Harmless when nothing was ever paused/scrubbed
    // (mlt_properties_set_int on a property that's already 0 is a no-op).
    if (m_consumer)
        m_consumer->set("refresh", 0);
    m_tractor->set_speed(speed);
    m_speed.store(speed);
    m_playing.store(speed != 0.0);
    applyResolvedScale();
}

void PlaybackController::pause()
{
    if (!m_tractor)
        return;
    // Snap to the frame on screen only when pausing *from playback* (E1
    // below). Already paused, the target is m_pausedPosition: a paused
    // seek/step hasn't been displayed yet, so snapping to the displayed
    // frame there threw away every step after the first in a quick
    // sequence (stepFrame() pauses before each step; found 2026-09-23 via
    // the new transport buttons, covered by "back-to-back frame steps
    // while paused each count").
    int displayed = m_playing.load() ? m_lastKnownFrame.load() : m_pausedPosition.load();
    Log::debug("[engine] pause() at frame " + std::to_string(displayed));
    m_pausedPosition.store(displayed);
    m_tractor->set_speed(0);
    m_speed.store(0.0);
    m_playing.store(false);
    if (m_consumer) {
        // Seek back to the frame actually on screen BEFORE purging (audit
        // E1, 2026-09-20). While playing, the consumer's read-ahead thread
        // has already pulled the producer up to `buffer` (25) frames past
        // what's displayed; purge() drops those queued frames but leaves
        // the producer's position where the read-ahead left it, so the
        // refresh frame below used to show that position instead --
        // measured: paused with frame 40 on screen, the refresh frame came
        // back as 72. kdenlive's VideoWidget::pause() seeks the producer
        // to the consumer's position before purging for the same reason;
        // doc 05's pause() spec had left that step out.
        m_tractor->seek(displayed);
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
        // stopped-on frame is crisp. tractor->profile() heap-allocates a
        // fresh wrapper on every call (see selectAndStartConsumer's
        // comment) -- called once here and owned, not twice and leaked;
        // this runs on every play/pause/scale-preference change.
        std::unique_ptr<Mlt::Profile> profile(m_tractor ? m_tractor->profile() : nullptr);
        bool isHighRes = profile && profile->height() >= 1080;
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

    // Only while actually playing forward: this frame-show path also
    // carries paused "refresh" frames (after seek(), stepFrame(), toEnd(),
    // a timeline click), and without this guard landing the playhead at or
    // past loop-out via any of those snapped it straight back to loop-in --
    // a paused seek should go exactly where asked, not be redirected by a
    // loop that's only meant to apply during playback. Guarding speed > 0
    // too: a reverse shuttle crossing loop-out on its way somewhere else
    // shouldn't wrap forward either.
    if (m_loopRange && m_playing.load() && m_speed.load() > 0.0 && data->position >= m_loopRange->second) {
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
