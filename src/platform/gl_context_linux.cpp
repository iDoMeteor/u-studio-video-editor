#include "platform/gl_context.h"

// Types and constants only: libEGL is loaded at run time, so a machine
// without it runs the CPU path and the build links nothing new (ADR-019).
// Without EGL_NO_X11, eglplatform.h pulls in Xlib and its macros.
#define EGL_NO_X11
#define MESA_EGL_NO_X11_HEADERS
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <dlfcn.h>

#include <cstdlib>
#include <mutex>

namespace ustudio::platform {

namespace {

// GL_RENDERER and GL_VERSION (GL/gl.h), and glGetString's type, without
// the GL headers.
constexpr unsigned kGlRenderer = 0x1F01;
constexpr unsigned kGlVersion = 0x1F02;
using GlGetString = const unsigned char *(*)(unsigned);

// The EGL entry points we use, looked up once per process.
struct Egl
{
    PFNEGLGETPROCADDRESSPROC getProcAddress = nullptr;
    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay = nullptr;
    PFNEGLQUERYDEVICESEXTPROC queryDevices = nullptr;
    PFNEGLINITIALIZEPROC initialize = nullptr;
    PFNEGLBINDAPIPROC bindApi = nullptr;
    PFNEGLCREATECONTEXTPROC createContext = nullptr;
    PFNEGLDESTROYCONTEXTPROC destroyContext = nullptr;
    PFNEGLMAKECURRENTPROC makeCurrent = nullptr;
    PFNEGLGETERRORPROC getError = nullptr;
    GlGetString glGetString = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    std::string error; // why there's no display, if there isn't
};

template <typename T> bool lookUp(void *library, const char *name, T &function)
{
    function = reinterpret_cast<T>(dlsym(library, name));
    return function != nullptr;
}

std::string hex(EGLint code)
{
    static const char digits[] = "0123456789abcdef";
    std::string text = "0x";
    for (int shift = 12; shift >= 0; shift -= 4)
        text += digits[(code >> shift) & 0xf];
    return text;
}

// The display is opened once and never terminated: other contexts may still
// use it, and drivers tear down badly at exit (kdenlive and Shotcut don't
// either).
Egl &egl()
{
    static Egl instance;
    static std::once_flag once;
    std::call_once(once, [] {
        const char *override = std::getenv("USTUDIO_EGL_LIBRARY");
        const char *name = override && *override ? override : "libEGL.so.1";
        void *library = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (!library) {
            instance.error = std::string("no EGL library (") + name + ")";
            return;
        }
        Egl e;
        if (!lookUp(library, "eglGetProcAddress", e.getProcAddress) ||
            !lookUp(library, "eglInitialize", e.initialize) || !lookUp(library, "eglBindAPI", e.bindApi) ||
            !lookUp(library, "eglCreateContext", e.createContext) ||
            !lookUp(library, "eglDestroyContext", e.destroyContext) ||
            !lookUp(library, "eglMakeCurrent", e.makeCurrent) || !lookUp(library, "eglGetError", e.getError)) {
            instance.error = "the EGL library lacks EGL 1.4 entry points";
            return;
        }
        e.getPlatformDisplay =
            reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(e.getProcAddress("eglGetPlatformDisplayEXT"));
        e.queryDevices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(e.getProcAddress("eglQueryDevicesEXT"));
        e.glGetString = reinterpret_cast<GlGetString>(e.getProcAddress("glGetString"));
        if (!e.getPlatformDisplay || !e.glGetString) {
            instance.error = "the EGL library has no eglGetPlatformDisplayEXT";
            return;
        }
        // Mesa's surfaceless platform needs nothing but a render node; the
        // device platform is how other drivers (NVIDIA's) offer the same.
        EGLDisplay display = e.getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        if (display == EGL_NO_DISPLAY || !e.initialize(display, nullptr, nullptr)) {
            display = EGL_NO_DISPLAY;
            EGLDeviceEXT device = nullptr;
            EGLint count = 0;
            if (e.queryDevices && e.queryDevices(1, &device, &count) && count > 0) {
                display = e.getPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, device, nullptr);
                if (display != EGL_NO_DISPLAY && !e.initialize(display, nullptr, nullptr))
                    display = EGL_NO_DISPLAY;
            }
        }
        if (display == EGL_NO_DISPLAY) {
            instance.error = "no EGL display (surfaceless or device) " + hex(e.getError());
            return;
        }
        e.display = display;
        instance = e;
    });
    return instance;
}

class EglContext final : public GlContext
{
  public:
    explicit EglContext(EGLContext context) : m_context(context) {}
    ~EglContext() override
    {
        egl().destroyContext(egl().display, m_context);
    }

    bool makeCurrent() override
    {
        // eglBindAPI is per thread (EGL 1.5 §3.7).
        Egl &e = egl();
        return e.bindApi(EGL_OPENGL_API) && e.makeCurrent(e.display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_context);
    }
    void release() override
    {
        egl().makeCurrent(egl().display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    std::string renderer() const override
    {
        return glString(kGlRenderer);
    }
    std::string version() const override
    {
        return glString(kGlVersion);
    }

    EGLContext handle() const
    {
        return m_context;
    }

  private:
    static std::string glString(unsigned name)
    {
        const unsigned char *text = egl().glGetString(name);
        return text ? reinterpret_cast<const char *>(text) : "";
    }

    EGLContext m_context;
};

} // namespace

std::unique_ptr<GlContext> GlContext::create(const GlContext *shareWith, std::string &error)
{
    Egl &e = egl();
    if (e.display == EGL_NO_DISPLAY) {
        error = e.error;
        return nullptr;
    }
    if (!e.bindApi(EGL_OPENGL_API)) {
        error = "EGL has no desktop OpenGL";
        return nullptr;
    }
    // movit needs 3.0 (compatibility is fine; a 3.2 core context would
    // also do). No config: the context never draws to a surface of its own
    // (EGL_KHR_no_config_context; Mesa and NVIDIA have it).
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
    const EGLContext share = shareWith ? static_cast<const EglContext *>(shareWith)->handle() : EGL_NO_CONTEXT;
    EGLContext context = e.createContext(e.display, EGL_NO_CONFIG_KHR, share, attributes);
    if (context == EGL_NO_CONTEXT) {
        error = "no OpenGL 3.0 context " + hex(e.getError());
        return nullptr;
    }
    return std::make_unique<EglContext>(context);
}

} // namespace ustudio::platform
