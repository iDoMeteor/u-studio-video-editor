// ADR-019 G2: our own GL context and the GPU probe. meson runs this twice:
// once as it is, where the probe must pass if EGL has a display (a machine
// or container without one skips), and once with USTUDIO_EGL_LIBRARY
// pointing nowhere, where it must fail cleanly and leave MLT on the CPU
// chain.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine/factory_policy.h"
#include "engine/gpu_probe.h"
#include "engine/producer_open.h"
#include "platform/gl_context.h"

#include <mlt++/Mlt.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

using namespace ustudio;
using namespace ustudio::engine;

namespace {

FactoryPolicy &sharedFactoryPolicy()
{
    static FactoryPolicy policy;
    return policy;
}

bool eglIsOverridden()
{
    const char *library = std::getenv("USTUDIO_EGL_LIBRARY");
    return library && *library;
}

// What the loader gives a new producer now: movit's chain or the CPU one.
bool loaderGoesGpu()
{
    Mlt::Profile profile;
    std::unique_ptr<Mlt::Producer> producer = openProducer(profile, "color:#000000", ProducerUse::GpuGraph);
    for (int i = 0; i < producer->filter_count(); ++i) {
        std::unique_ptr<Mlt::Filter> filter(producer->filter(i));
        const char *service = filter->get("mlt_service");
        if (service && std::string(service).starts_with("movit."))
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("a GL context is made, shared and current on one thread at a time")
{
    if (eglIsOverridden())
        return;
    std::string error;
    std::unique_ptr<platform::GlContext> first = platform::GlContext::create(nullptr, error);
    if (!first) {
        MESSAGE("no GL here (" << error << "); nothing to check");
        return;
    }
    std::unique_ptr<platform::GlContext> shared = platform::GlContext::create(first.get(), error);
    REQUIRE(shared);
    REQUIRE(first->makeCurrent());
    CHECK_FALSE(first->renderer().empty());
    CHECK_FALSE(first->version().empty());
    // Another thread can hold the shared context meanwhile.
    std::string otherRenderer;
    std::thread other([&] {
        if (shared->makeCurrent()) {
            otherRenderer = shared->renderer();
            shared->release();
        }
    });
    other.join();
    CHECK(otherRenderer == first->renderer());
    first->release();
    CHECK(first->renderer().empty()); // nothing current: no GL strings
}

TEST_CASE("the probe passes where there's GL, and leaves MLT on the CPU chain")
{
    if (eglIsOverridden())
        return;
    sharedFactoryPolicy();
    const GpuProbeResult result = probeGpu();
    if (!result.ok && result.renderer.empty()) {
        MESSAGE("no GL here (" << result.message << "); nothing to check");
        return;
    }
    INFO(result.message);
    CHECK(result.ok);
    CHECK_FALSE(result.renderer.empty());
    CHECK_FALSE(loaderGoesGpu());
    // Twice in one process, as a toggled setting would.
    CHECK(probeGpu().ok);
    CHECK_FALSE(loaderGoesGpu());
}

TEST_CASE("without EGL the probe fails cleanly")
{
    if (!eglIsOverridden())
        return;
    sharedFactoryPolicy();
    std::string error;
    CHECK_FALSE(platform::GlContext::create(nullptr, error));
    CHECK(error.starts_with("no EGL library"));
    const GpuProbeResult result = probeGpu();
    CHECK_FALSE(result.ok);
    CHECK(result.message.starts_with("no EGL library"));
    CHECK(result.renderer.empty());
    CHECK_FALSE(loaderGoesGpu());
}
