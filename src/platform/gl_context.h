#pragma once

// ADR-017: see process.h. ADR-019: the GL context movit renders with is our
// own, not a toolkit's (kdenlive and Shotcut take Qt's, which ADR-007 bans).
// It needs no window or display-server connection.

#include <memory>
#include <string>

namespace ustudio::platform {

class GlContext
{
  public:
    // A desktop OpenGL 3.0+ context sharing objects with `shareWith` (may be
    // null), current on no thread. Null with `error` set when there's none
    // to be had: no EGL library, no display, no driver. Linux: EGL loaded at
    // run time (USTUDIO_EGL_LIBRARY overrides "libEGL.so.1"), the surfaceless
    // Mesa platform first, then the first EGL device.
    static std::unique_ptr<GlContext> create(const GlContext *shareWith, std::string &error);
    virtual ~GlContext() = default;

    // Current on the calling thread until release(), or until the thread
    // makes another current. False (and nothing current) on failure.
    virtual bool makeCurrent() = 0;
    virtual void release() = 0;

    // GL_RENDERER and GL_VERSION, read with the context current on the
    // calling thread ("" otherwise).
    virtual std::string renderer() const = 0;
    virtual std::string version() const = 0;

  protected:
    GlContext() = default;
    GlContext(const GlContext &) = delete;
    GlContext &operator=(const GlContext &) = delete;
};

} // namespace ustudio::platform
