#include "engine/gpu_probe.h"

#include "core/log.h"
#include "engine/producer_open.h"
#include "platform/gl_context.h"

#include <mlt++/Mlt.h>

#include <cstdlib>
#include <memory>

namespace ustudio::engine {

namespace Log = ustudio::core::Log;

namespace {

constexpr int kWidth = 320, kHeight = 180;
// A colour whose channels all differ, placed in the top-left quadrant.
constexpr int kRed = 0x20, kGreen = 0x80, kBlue = 0xc0;
constexpr int kTolerance = 3; // the GPU path is within 1-2 levels of its source (docs/developer/notes/gpu.md)

bool near(const uint8_t *pixel, int r, int g, int b)
{
    return std::abs(pixel[0] - r) <= kTolerance && std::abs(pixel[1] - g) <= kTolerance &&
           std::abs(pixel[2] - b) <= kTolerance;
}

std::string describe(const uint8_t *pixel)
{
    return std::to_string(pixel[0]) + "," + std::to_string(pixel[1]) + "," + std::to_string(pixel[2]);
}

// Renders one frame through the GPU graph; "" when its pixels are right.
std::string renderAndCheck(Mlt::Profile &profile)
{
    Mlt::Tractor tractor(profile);
    // Through the loader, so each gets movit's normalisers (the Live chain).
    std::unique_ptr<Mlt::Producer> black = openProducer(profile, "color:#000000", ProducerUse::Live);
    std::unique_ptr<Mlt::Producer> picture = openProducer(profile, "color:#2080c0", ProducerUse::Live);
    if (!black->is_valid() || !picture->is_valid())
        return "couldn't open colour producers";
    Mlt::Filter rect(profile, "movit.rect");
    Mlt::Transition overlay(profile, "movit.overlay");
    if (!rect.is_valid() || !overlay.is_valid())
        return "no movit.rect or movit.overlay";
    // Service and property names from /usr/share/mlt-7/movit/*.yml.
    rect.set("rect", "0/0:160x90");
    rect.set("distort", 1);
    picture->attach(rect);
    tractor.set_track(*black, 0);
    tractor.set_track(*picture, 1);
    tractor.plant_transition(overlay, 0, 1);

    std::unique_ptr<Mlt::Frame> frame(tractor.get_frame());
    mlt_image_format format = mlt_image_rgba;
    int width = kWidth, height = kHeight;
    const uint8_t *image = frame ? frame->get_image(format, width, height) : nullptr;
    if (!image || format != mlt_image_rgba || width != kWidth || height != kHeight)
        return "the GPU graph returned no RGBA frame";
    const uint8_t *inside = image + (45 * kWidth + 80) * 4;
    const uint8_t *outside = image + (135 * kWidth + 240) * 4;
    if (!near(inside, kRed, kGreen, kBlue) || !near(outside, 0, 0, 0))
        return "wrong pixels: " + describe(inside) + " inside, " + describe(outside) + " outside";
    return {};
}

} // namespace

GpuProbeResult probeGpu()
{
    GpuProbeResult result;
    std::unique_ptr<platform::GlContext> context = platform::GlContext::create(nullptr, result.message);
    if (!context)
        return result;
    if (!context->makeCurrent()) {
        result.message = "couldn't make the GL context current";
        return result;
    }
    result.renderer = context->renderer();
    result.version = context->version();

    Mlt::Profile profile;
    profile.set_width(kWidth);
    profile.set_height(kHeight);
    profile.set_frame_rate(30, 1);
    profile.set_sample_aspect(1, 1);
    profile.set_display_aspect(16, 9);
    profile.set_progressive(1);
    profile.set_colorspace(709);
    {
        Mlt::Filter manager(profile, "glsl.manager");
        if (!manager.is_valid()) {
            result.message = "MLT has no movit module";
        } else {
            manager.fire_event("init glsl");
            if (!manager.get_int("glsl_supported"))
                result.message = "movit couldn't initialise on " + result.renderer;
            else
                result.message = renderAndCheck(profile);
            // GL objects go while the context is still current.
            manager.fire_event("close glsl");
        }
    }
    // Creating the manager switched every later loader producer to movit
    // (docs/developer/notes/gpu.md); clearing the global switches it back.
    mlt_properties_set_data(mlt_global_properties(), "glslManager", nullptr, 0, nullptr, nullptr);
    context->release();
    result.ok = result.message.empty();
    Log::info(std::string("[gpu] probe ") + (result.ok ? "passed" : "failed: " + result.message) + " (" +
              result.renderer + ", " + result.version + ")");
    return result;
}

} // namespace ustudio::engine
