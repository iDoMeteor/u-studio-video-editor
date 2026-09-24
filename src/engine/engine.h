#pragma once

#include "core/model/signal.h"
#include "core/model/types.h"
#include "engine/dispatcher.h"
#include "engine/preview_scale.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ustudio::engine {

// doc 19 MT2 / ADR-016: the main thread's handle on the engine thread.
//
// One std::jthread owns EngineSync (graph builds), the master producers,
// the tractor and PlaybackController (the consumer). The main thread sends
// it commands through a queue and never waits on it, except in shutdown().
// State comes back through MainThreadDispatcher and is mirrored here, so
// currentFrame(), totalFrames(), fps() and friends are cheap synchronous
// reads. No MLT in this header: src/app/ includes it (app-boundary-check).
//
// Mirror rules (why paused seeks and quick steps still land exactly):
// - seek/step/home/end move the mirrored position at call time, so a second
//   step computes from the first step's target, not from a frame that
//   hasn't come back yet;
// - every command carries a sequence number, and the engine tags each state
//   report with the last one it processed; while paused, a reported
//   position is applied only if nothing newer has been sent since, so an
//   echo of an earlier step can't pull the playhead back;
// - while playing, delivered frames move it.
//
// Snapshots share the command queue, so a seek sent after an edit lands on
// the new graph; consecutive snapshots collapse to the newest (latest wins:
// a burst of edits costs at most one build beyond the last one).
class Engine
{
  public:
    using FrameCallback = std::function<void(std::vector<uint8_t> rgba, int width, int height, int frameNumber)>;

    Engine(std::shared_ptr<const core::Project> project, PreviewScale previewScale);
    // Calls shutdown() if nothing has yet.
    ~Engine();
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    // A new state of the same project (EngineSync::setProject()).
    void publish(std::shared_ptr<const core::Project> project);
    // A different project (EngineSync::reset()).
    void reset(std::shared_ptr<const core::Project> project);
    void setPreviewScale(PreviewScale scale);

    // Main thread, once per displayed frame.
    void setFrameCallback(FrameCallback callback);

    void play(double speed = 1.0);
    void pause();
    void togglePlay();
    void seek(int frame);
    void stepFrame(int delta);
    void toHome();
    void toEnd();
    void setLoopRange(std::optional<std::pair<int, int>> range);
    std::optional<std::pair<int, int>> loopRange() const
    {
        return m_loopRange;
    }
    void setVolume(double volume);
    double volume() const
    {
        return m_volume;
    }

    bool isPlaying() const
    {
        return m_playing;
    }
    double speed() const
    {
        return m_speed;
    }
    int currentFrame() const
    {
        return m_position;
    }
    int totalFrames() const
    {
        return m_totalFrames;
    }
    double fps() const
    {
        return m_fps;
    }
    std::string backendName() const
    {
        return m_backend;
    }

    // Sends shutdown and joins the engine thread: the consumer is stopped
    // and every MLT object dropped when this returns (post-M3 audit P2:
    // before Factory::close()). The only call that waits. Idempotent.
    void shutdown();

    // Main thread, after the engine thread has swapped in a new graph
    // (totalFrames()/fps() are already updated).
    core::Signal<> rebuilt;
    // Main thread: an asset's file couldn't be opened (EngineSync).
    core::Signal<const std::string &> mediaUnavailable;

    // Tests only: blocks until the engine thread has run everything sent so
    // far and its state reports have been applied here (it pumps the GLib
    // main context while waiting). Never called by the app.
    void syncForTesting();

  private:
    struct State
    {
        uint64_t seq = 0;
        int position = 0;
        bool playing = false;
        double speed = 0.0;
        int totalFrames = 0;
        double fps = 0.0;
        std::string backend;
    };
    class Thread;

    uint64_t send(std::function<void(Thread &)> command);
    void applyState(const State &state, bool graphRebuilt);
    void onFrame(std::vector<uint8_t> rgba, int width, int height, int position);
    int clampFrame(int frame) const;

    // Before m_thread: the thread's constructor takes a weak reference to it.
    MainThreadDispatcher::LifetimeToken m_lifetime = MainThreadDispatcher::makeToken();
    std::unique_ptr<Thread> m_thread;
    FrameCallback m_frameCallback;

    // The mirror (main thread only).
    uint64_t m_sent = 0;    // last command sequence number sent
    uint64_t m_applied = 0; // last one the engine reported back
    int m_position = 0;
    bool m_playing = false;
    double m_speed = 0.0;
    int m_totalFrames = 0;
    double m_fps = 0.0;
    std::string m_backend;
    double m_volume = 1.0;
    std::optional<std::pair<int, int>> m_loopRange;
};

} // namespace ustudio::engine
