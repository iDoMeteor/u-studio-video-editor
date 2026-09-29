// The frame renderer while the GPU pipeline is on (ADR-019): a glsl.manager
// switches the default loader to movit's normalisers for the whole process,
// and the renderer's worker thread has no GL context. Alone in its own
// executable, since the manager outlives the session and would turn the
// engine suite's other graphs into movit ones. Skips where there's no GL.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine/factory_policy.h"
#include "engine/frame_renderer.h"
#include "engine/gpu_session.h"

#include <memory>
#include <string>
#include <thread>

using namespace ustudio;
using namespace ustudio::effects;

TEST_CASE("FrameRenderer: a push's tile renders off the GL thread while the GPU pipeline is on")
{
    static engine::FactoryPolicy policy;
    std::string error;
    std::shared_ptr<engine::GpuSession> session = engine::GpuSession::acquire(error);
    if (!session) {
        MESSAGE("skipped, no GPU session: " << error);
        return;
    }
    // The push: an affine filter moving the outgoing clip off (its own
    // background producer, which the default loader would make a movit one)
    // and the affine transition bringing the incoming one on.
    FrameRequest request;
    request.resource = "color:0xff0000ff";
    request.profile.fps = {30, 1};
    request.clipIn = 0;
    request.clipOut = 19;
    request.width = 160;
    request.height = 90;
    FrameRequest::Transition push;
    push.resource = "color:0x0000ffff";
    push.in = 0;
    push.out = 19;
    push.position = 10;
    push.params = {{"video.service", std::string("affine"), {}},
                   {"video.rect", std::string("ramp:100% 0% 100% 100%|0% 0% 100% 100%"), {}},
                   {"a.0.service", std::string("affine"), {}},
                   {"a.0.transition.rect", std::string("ramp:0% 0% 100% 100%|-100% 0% 100% 100%"), {}}};
    request.transition = push;
    RenderedFrame frame;
    std::thread worker([&] { frame = FrameRenderer::renderNow(request); });
    worker.join();
    REQUIRE(frame.width == 160);
    auto at = [&](int x, int channel) {
        return frame.rgba[(static_cast<size_t>(45) * 160 + static_cast<size_t>(x)) * 4 + static_cast<size_t>(channel)];
    };
    CHECK(at(10, 0) > 200);  // the outgoing red, pushed to the left half
    CHECK(at(150, 2) > 200); // the incoming blue on the right
}
