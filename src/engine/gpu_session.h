#pragma once

// ADR-019: the GPU pipeline's process-wide state, for as long as it's on.
// Creating one makes a GL context of our own, creates MLT's glsl.manager
// and initialises movit with the context current on the calling thread,
// then releases it. From then on the default loader gives every producer
// movit's normalisers (docs/developer/notes/gpu.md), so EngineSync must
// reopen its masters (EngineSync::setPipeline()). The consumer's render
// thread takes the context through renderThreadStarted()/Stopped().
// Destroying it clears the global switch again.
//
// One per process, shared: the preview (engine::Engine) holds it while the
// GPU pipeline is on, and an export started meanwhile (renderProject())
// takes a reference too and renders with a context of its own in the same
// share group, so the session outlives a preview that switches back to the
// CPU mid-export. The last reference goes on the engine thread or the
// export's thread, never the main thread (whose GL context is GTK's).

#include <memory>
#include <mutex>
#include <string>

namespace ustudio::platform {
class GlContext;
}

namespace Mlt {
class Filter;
}

namespace ustudio::engine {

class GpuSession
{
  public:
    // The live session, or a new one. Null with `error` set when there's
    // no GL context or movit won't initialise; the process is left on the
    // CPU chain.
    static std::shared_ptr<GpuSession> acquire(std::string &error);
    // The live session, or null (never starts one): what an export asks.
    static std::shared_ptr<GpuSession> current();
    // The consumer that used the context must be stopped first.
    ~GpuSession();
    GpuSession(const GpuSession &) = delete;
    GpuSession &operator=(const GpuSession &) = delete;

    // On the consumer's render thread (consumer-thread-started/-stopped,
    // PlaybackController::setRenderThreadHooks()). False when the context
    // couldn't be made current: frames from that thread would fail.
    bool renderThreadStarted();
    void renderThreadStopped();

    // Around rendering on the consumer's own thread (PlaybackController::
    // setFrameShowHooks()). MLT's read-ahead thread passes on a frame it
    // skipped for lateness unrendered, and sdl2_audio shows such frames
    // without checking (the paused refresh; the frames still queued when
    // it stops); get_image() then runs the whole movit graph on sdl2's
    // thread, which needs a context of its own (the render thread holds the
    // first). One thread at a time. docs/developer/notes/gpu.md.
    bool frameShowEnter();
    void frameShowLeave();

    const std::string &renderer() const
    {
        return m_renderer;
    }

    // Another context in this session's share group (movit's texture pool
    // is shared), for an export's render thread. Null with `error` set.
    std::unique_ptr<platform::GlContext> sharedContext(std::string &error) const;

    // The hardware decode API the preview uses ("" for software), so an
    // export decodes the same way. Any thread.
    void setHardwareDecodeApi(const std::string &api);
    std::string hardwareDecodeApi() const;

  private:
    GpuSession() = default;
    static std::unique_ptr<GpuSession> start(std::string &error);
    mutable std::mutex m_mutex;
    std::string m_hardwareDecodeApi;

    std::unique_ptr<platform::GlContext> m_context;
    std::unique_ptr<platform::GlContext> m_showContext; // same share group
    std::unique_ptr<Mlt::Filter> m_manager;
    std::string m_renderer;
};

} // namespace ustudio::engine
