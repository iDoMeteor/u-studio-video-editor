#pragma once

// ADR-019: the GPU pipeline's process-wide state, for as long as it's on.
// Creating one makes a GL context of our own, creates MLT's glsl.manager
// and initialises movit with the context current on the calling thread,
// then releases it. From then on the default loader gives every producer
// movit's normalisers (docs/developer/notes/gpu.md), so EngineSync must
// reopen its masters (EngineSync::setPipeline()). The consumer's render
// thread takes the context through renderThreadStarted()/Stopped().
// Destroying it clears the global switch again. One at a time per process;
// engine thread only, apart from the two render-thread calls.

#include <memory>
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
    // Null with `error` set when there's no GL context or movit won't
    // initialise; the process is left on the CPU chain.
    static std::unique_ptr<GpuSession> start(std::string &error);
    // The consumer that used the context must be stopped first.
    ~GpuSession();
    GpuSession(const GpuSession &) = delete;
    GpuSession &operator=(const GpuSession &) = delete;

    // On the consumer's render thread (consumer-thread-started/-stopped,
    // PlaybackController::setRenderThreadHooks()). False when the context
    // couldn't be made current: frames from that thread would fail.
    bool renderThreadStarted();
    void renderThreadStopped();

    const std::string &renderer() const
    {
        return m_renderer;
    }

  private:
    GpuSession() = default;

    std::unique_ptr<platform::GlContext> m_context;
    std::unique_ptr<Mlt::Filter> m_manager;
    std::string m_renderer;
};

} // namespace ustudio::engine
