#pragma once

#include "dispatcher.h"

#include <mlt++/Mlt.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ustudio::engine {

// Owns an Mlt::Consumer and drives playback from it (ADR-002, doc 05) --
// replaces v1/M1's MltEngine, which pulled frames on a hand-rolled worker
// thread and paced against a blocking PulseAudio write. That approach had
// a constant ~150ms A/V offset, drifted on silent clips, and couldn't drop
// frames; a real MLT consumer (sdl2_audio, falling back to rtaudio, then
// null) owns the clock instead and solves all three.
//
// Threading: every public method here is main-thread-only (GTK signal
// handlers, same as before). The one exception is the consumer's
// "consumer-frame-show" event, delivered on an MLT-owned thread; that
// handler does the minimum possible (copy the frame into a lock-protected
// single-slot mailbox, post a coalesced main-thread wakeup via
// MainThreadDispatcher) and touches nothing else. There is no
// project-wide mutex anymore: control operations (play/pause/seek) only
// ever run on the main thread, so they don't need one.
class PlaybackController
{
  public:
    using FrameCallback = std::function<void(std::vector<uint8_t> rgba, int width, int height, int frameNumber)>;

    enum class PreviewScale
    {
        Auto,    // 1.0 while paused, 0.5 while playing a 1080p+ profile (doc 05)
        Full,
        Half,
        Quarter,
    };

    PlaybackController();
    ~PlaybackController();

    PlaybackController(const PlaybackController &) = delete;
    PlaybackController &operator=(const PlaybackController &) = delete;

    // Points playback at a new tractor, after any model change that
    // required EngineSync::rebuildAll(). If a consumer is already running
    // against the same profile (the common case: this tractor is a
    // rebuild after an ordinary edit, not a project load), it is
    // reconnected in place -- no stop/restart, so editing never clicks or
    // drops the audio device. If the profile differs (a project load with
    // a different resolution/fps), the consumer is stopped and a fresh one
    // selected against the new profile (doc 05: "changing the sequence
    // profile rebuilds everything"). Preserves the current playhead
    // position and play/pause state across either path.
    void setTractor(std::shared_ptr<Mlt::Tractor> tractor);
    void setFrameCallback(FrameCallback cb);

    // speed: 1.0 = normal forward. J/K/L shuttle uses {-8,-4,-2,-1,-0.5,
    // -0.25, 0.25, 0.5, 1, 2, 4, 8}; sdl2_audio/rtaudio handle reverse and
    // speed change themselves (audio mutes above |2x|, an MLT default this
    // class leaves alone -- doc 05).
    void play(double speed = 1.0);
    void pause();
    void togglePlay();
    bool isPlaying() const
    {
        return m_playing.load();
    }
    double speed() const
    {
        return m_speed.load();
    }

    void seek(int frame);
    void stepFrame(int delta); // pause, then seek(current + delta)
    void toHome();              // seek(0)
    void toEnd();                // seek(totalFrames() - 1)

    // std::nullopt clears the loop. Playback wraps to `in` once position
    // reaches `out` (checked on the main thread from the frame-show
    // wakeup, doc 05 -- a one-frame overshoot before wrapping is accepted).
    void setLoopRange(std::optional<std::pair<int, int>> range);
    std::optional<std::pair<int, int>> loopRange() const
    {
        return m_loopRange;
    }

    // 0.0..1.0 (consumer's own "volume" property; sdl2_audio/rtaudio both
    // support it, verified against their .yml metadata).
    void setVolume(double volume);
    double volume() const
    {
        return m_volume.load();
    }

    void setPreviewScale(PreviewScale scale);
    PreviewScale previewScale() const
    {
        return m_previewScalePreference;
    }

    // Source of truth (doc 05): while playing, the position carried by the
    // most recent frame-show event; while paused, whatever was last sought
    // to. Never tractor->position() -- that runs ahead of what's on screen
    // by the consumer's prefetch buffer.
    int currentFrame() const;
    int totalFrames() const;
    double fps() const;

    // Empty until the first setTractor() picks one; "null" if only that
    // last resort could be opened (persistent "no audio" banner territory).
    std::string backendName() const
    {
        return m_backendName;
    }

    // Stops the consumer before Factory::close() runs on quit (main.cpp's
    // "shutdown" handler). Can't just be the destructor: AppWindow (and
    // everything it owns, including this) is intentionally never destroyed
    // on normal quit -- GTK owns/destroys the widget tree instead -- so
    // this needs an explicit call from a real shutdown hook. Idempotent.
    void shutdown();

  private:
    struct FrameData
    {
        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;
        int position = 0;
    };

    // Single-slot mailbox (doc 05's LatestFrameSlot): the consumer thread
    // store()s, the main thread take()s. If the main loop falls behind,
    // frames are overwritten, never queued.
    class LatestFrameSlot
    {
      public:
        void store(FrameData data);
        std::optional<FrameData> take();

      private:
        std::mutex m_mutex;
        std::optional<FrameData> m_data;
    };

    bool selectAndStartConsumer(Mlt::Tractor &tractor);
    void applyResolvedScale();
    void applyVolumeToConsumer();
    static void frameShowTrampoline(mlt_properties owner, void *self, mlt_event_data data);
    void handleFrameShow(const Mlt::EventData &eventData);
    void drainSlot();

    std::shared_ptr<Mlt::Tractor> m_tractor;
    std::unique_ptr<Mlt::Consumer> m_consumer;
    std::unique_ptr<Mlt::Event> m_frameShowEvent;
    Mlt::Profile *m_consumerProfile = nullptr; // identity only, non-owning: which profile m_consumer was built for

    std::string m_backendName;
    FrameCallback m_callback;

    LatestFrameSlot m_slot;
    MainThreadDispatcher::LifetimeToken m_lifetimeToken = MainThreadDispatcher::makeToken();
    std::atomic<bool> m_drainQueued{false};

    std::atomic<bool> m_playing{false};
    std::atomic<double> m_speed{1.0};
    std::atomic<int> m_lastKnownFrame{0}; // updated from the consumer thread; valid while playing
    std::atomic<int> m_pausedPosition{0}; // last explicit seek target; valid while paused
    std::atomic<double> m_volume{1.0};
    PreviewScale m_previewScalePreference = PreviewScale::Auto;
    std::optional<std::pair<int, int>> m_loopRange;
};

} // namespace ustudio::engine
