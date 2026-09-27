#pragma once

// ADR-019 point 6: can this machine run the GPU pipeline? The editor asks
// `u-studio-render --gpu-probe` in a child process, so a GL driver that
// crashes takes down only the child.

#include <string>

namespace ustudio::engine {

struct GpuProbeResult
{
    bool ok = false;
    std::string renderer; // GL_RENDERER, when a context was made
    std::string version;  // GL_VERSION (Mesa's includes its own version)
    std::string message;  // why not, when !ok
};

// Makes a GL context current on the calling thread, initialises movit and
// renders a known picture through movit.rect and movit.overlay, checking
// its pixels. Leaves the process as it found it (no glsl.manager, nothing
// current), but initialising movit is process-wide while it runs, so only
// the render tool and tests call this, never the editor.
GpuProbeResult probeGpu();

} // namespace ustudio::engine
