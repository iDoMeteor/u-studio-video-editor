#include "platform/gl_context.h"

// Built where the EGL headers weren't found: no GPU path, the CPU one runs.

namespace ustudio::platform {

std::unique_ptr<GlContext> GlContext::create(const GlContext *, std::string &error)
{
    error = "built without EGL";
    return nullptr;
}

} // namespace ustudio::platform
