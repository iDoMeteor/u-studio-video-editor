#include "engine/gpu_session.h"

#include "core/log.h"
#include "platform/gl_context.h"

#include <mlt++/Mlt.h>

namespace ustudio::engine {

namespace Log = ustudio::core::Log;

std::unique_ptr<GpuSession> GpuSession::start(std::string &error)
{
    std::unique_ptr<GpuSession> session(new GpuSession);
    session->m_context = platform::GlContext::create(nullptr, error);
    if (!session->m_context)
        return nullptr;
    if (!session->m_context->makeCurrent()) {
        error = "couldn't make the GL context current";
        return nullptr;
    }
    session->m_renderer = session->m_context->renderer();
    // The manager's profile is irrelevant: it renders nothing itself.
    Mlt::Profile profile;
    session->m_manager = std::make_unique<Mlt::Filter>(profile, "glsl.manager");
    if (!session->m_manager->is_valid()) {
        session->m_manager.reset();
        session->m_context->release();
        error = "MLT has no movit module";
        return nullptr;
    }
    // movit initialises once per process, against whatever context is
    // current; the render thread later uses this same context.
    session->m_manager->fire_event("init glsl");
    const bool supported = session->m_manager->get_int("glsl_supported") != 0;
    session->m_context->release();
    if (!supported) {
        error = "movit couldn't initialise on " + session->m_renderer;
        return nullptr; // the destructor clears the global switch
    }
    Log::info("[gpu] GPU pipeline on (" + session->m_renderer + ")");
    return session;
}

GpuSession::~GpuSession()
{
    if (m_manager) {
        // GL objects go while the context is current (no consumer is
        // running, so no other thread holds it).
        if (m_context && m_context->makeCurrent()) {
            m_manager->fire_event("close glsl");
            m_context->release();
        }
        m_manager.reset();
        // The manager keeps itself alive (never freed; lsan.supp); clearing
        // the global is what returns the loader to the CPU chain.
        mlt_properties_set_data(mlt_global_properties(), "glslManager", nullptr, 0, nullptr, nullptr);
        Log::info("[gpu] GPU pipeline off");
    }
}

bool GpuSession::renderThreadStarted()
{
    if (m_context->makeCurrent())
        return true;
    Log::error("[gpu] the render thread couldn't make the GL context current");
    return false;
}

void GpuSession::renderThreadStopped()
{
    m_context->release();
}

} // namespace ustudio::engine
