#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <glib.h>

// Forward-declared so nothing outside this module needs an MLT include.
namespace Mlt {
class Tractor;
} // namespace Mlt

// Forward-declared so nothing outside this module needs a PulseAudio include.
struct pa_simple;

namespace ustudio::engine {

// Playback only: pulls frames from a Tractor someone else builds and owns
// (EngineSync, doc 05) and pushes them to a frame callback + PulseAudio.
// MltEngine used to also own the tractor and every editing primitive
// (moveClip/trimClipStart/splitAt/...); those are now Model mutators and
// core::Commands (doc 03/04), with EngineSync (doc 05) projecting the
// model into the Tractor this class plays. setTractor() hands over a new
// one after each model change; the worker thread only ever reads whatever
// m_tractor currently points to, under m_mltMutex, so a handoff mid-pull
// is safe as long as the caller doesn't destroy the tractor it's replacing
// — shared_ptr, not a raw pointer, so EngineSync rebuilding and dropping
// its own reference doesn't free a tractor this class might still be
// mid-get_frame() on.
//
// Playback model: rather than a push-style custom Mlt::Consumer, a dedicated
// worker thread pulls one frame at a time from the Tractor (which is itself
// a Producer), and hands the decoded RGBA buffer to the GTK main thread via
// g_idle_add(). MLT decodes on its own thread; GTK widgets may only be
// touched from the main thread, so every frame crosses that boundary through
// the GLib main loop rather than a raw callback. Playback is paced against a
// wall-clock schedule (re-anchored on every play/resume and every seek) —
// audio is written via a blocking PulseAudio call but is not trusted as the
// clock, since a buffer underrun makes the next write return instantly and
// would otherwise cause a runaway-fast feedback loop.
class MltEngine
{
  public:
    // Invoked already marshaled onto the GLib main thread — safe to touch
    // GTK widgets directly from inside the callback.
    using FrameCallback = std::function<void(std::vector<uint8_t> rgba, int width, int height, int frameNumber)>;

    MltEngine();
    ~MltEngine();

    MltEngine(const MltEngine &) = delete;
    MltEngine &operator=(const MltEngine &) = delete;

    // Points playback at a new tractor (after any model change that
    // required EngineSync::rebuildAll()), preserving the current playhead
    // position (clamped to the new tractor's length). Safe to call from
    // the main thread at any time, including during playback.
    void setTractor(std::shared_ptr<Mlt::Tractor> tractor);

    void play();
    void pause();
    void togglePlay();
    bool isPlaying() const
    {
        return m_playing.load();
    }

    // Non-blocking: requests the worker thread seek and deliver one frame.
    void seek(int frame);

    int currentFrame() const
    {
        return m_lastKnownFrame.load();
    }
    int totalFrames() const;
    double fps() const;

    void setFrameCallback(FrameCallback cb);

  private:
    struct PendingFrame
    {
        MltEngine *engine;
        std::vector<uint8_t> rgba;
        int width;
        int height;
        int frameNumber;
    };

    void pullLoopMain();
    static gboolean deliverOnMainThread(gpointer data);

    std::shared_ptr<Mlt::Tractor> m_tractor;
    pa_simple *m_audioStream = nullptr;

    mutable std::mutex m_mltMutex;
    std::thread m_worker;
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_quit{false};
    std::atomic<int> m_seekRequest{-1};
    std::atomic<int> m_lastKnownFrame{0};
    // Mirrors m_tractor->get_length(), updated whenever it changes
    // (setTractor). Lets seek()/totalFrames() answer without touching
    // m_mltMutex — that mutex can be held by the worker thread for a real
    // stretch during an expensive seek on a large file, and a UI callback
    // blocking on it (e.g. every "value-changed" tick while dragging the
    // seek scale during playback) is what froze the app.
    std::atomic<int> m_totalFramesCache{0};
    std::atomic<double> m_fpsCache{30.0};

    FrameCallback m_callback;
};

} // namespace ustudio::engine
