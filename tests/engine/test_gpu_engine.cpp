// ADR-019 G3: the GPU pipeline through engine::Engine and the real
// consumer. The render thread takes the session's context, frames reach
// the frame callback with the right pixels, the pipeline switches back to
// the CPU while playing, and shutdown with it on is clean. Skips where
// there's no GL.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/model/model.h"
#include "core/model/transform.h"
#include "engine/engine.h"
#include "engine/factory_policy.h"

#include <glib.h>

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace ustudio::core;
using namespace ustudio::engine;

namespace {

FactoryPolicy &sharedFactoryPolicy()
{
    static FactoryPolicy policy;
    return policy;
}

template <class Done> bool pumpUntil(Done done, std::chrono::milliseconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_iteration(nullptr, FALSE)) {
        }
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// Red on V1 and, above it, blue placed in the top-left quadrant, 300 frames.
Model makeModel()
{
    Model model = Model::createEmpty();
    TrackId lower = model.addTrack(Track::Kind::Video, 0, "V1");
    TrackId upper = model.addTrack(Track::Kind::Video, 0, "V2");
    auto colour = [&](const char *path) {
        Asset asset;
        asset.path = path;
        asset.info.hasVideo = true;
        asset.info.lengthInSequenceFrames = 100'000;
        return model.addAsset(asset);
    };
    model.insertClip(lower, colour("color:#c02020"), 0, 0, 299);
    ClipId top = model.insertClip(upper, colour("color:#2040c0"), 0, 0, 299);
    Transform t;
    t.bounds = Transform::Bounds::None;
    t.x.value = 480;
    t.y.value = 270;
    t.width.value = 960;
    t.height.value = 540;
    model.setClipTransform(top, t);
    return model;
}

struct Frames
{
    int count = 0;
    std::vector<uint8_t> last;
    int width = 0, height = 0;
    void attach(Engine &engine)
    {
        engine.setFrameCallback([this](std::vector<uint8_t> rgba, int w, int h, int) {
            ++count;
            last = std::move(rgba);
            width = w;
            height = h;
        });
    }
    // The last frame at (fx, fy), as fractions of its size.
    const uint8_t *at(double fx, double fy) const
    {
        const size_t x = static_cast<size_t>(fx * width), y = static_cast<size_t>(fy * height);
        return last.data() + (y * static_cast<size_t>(width) + x) * 4;
    }
    bool right() const
    {
        if (last.empty())
            return false;
        const uint8_t *blue = at(0.25, 0.25), *red = at(0.75, 0.75);
        return blue[2] > 150 && blue[0] < 80 && red[0] > 150 && red[2] < 80;
    }
};

struct GpuState
{
    std::optional<bool> on;
    std::string detail;
    void attach(Engine &engine)
    {
        engine.gpuChanged.connect([this](bool isOn, const std::string &why) {
            on = isOn;
            detail = why;
        });
    }
};

} // namespace

TEST_CASE("Engine: the GPU pipeline plays through the consumer and switches back while playing")
{
    sharedFactoryPolicy();
    Model model = makeModel();
    Engine engine(model.snapshot(), PreviewScale::Full);
    Frames frames;
    frames.attach(engine);
    GpuState gpu;
    gpu.attach(engine);
    REQUIRE(engine.syncForTesting());

    engine.setGpuPipeline(true, {});
    REQUIRE(pumpUntil([&] { return gpu.on.has_value(); }, std::chrono::seconds(10)));
    if (!*gpu.on) {
        MESSAGE("no GPU pipeline here (" << gpu.detail << ")");
        engine.shutdown();
        return;
    }
    CHECK(engine.gpuPipeline());
    CHECK_FALSE(gpu.detail.empty()); // the renderer

    // A paused seek shows the GPU graph's frame.
    frames.last.clear();
    engine.seek(10);
    REQUIRE(pumpUntil([&] { return frames.right(); }, std::chrono::seconds(5)));

    // Playing: frames keep coming, and they stay right.
    const int before = frames.count;
    engine.play();
    REQUIRE(pumpUntil([&] { return frames.count >= before + 20; }, std::chrono::seconds(5)));
    CHECK(frames.right());

    // Back to the CPU mid-play: the engine stops the consumer, drops the
    // session and plays the CPU graph.
    gpu.on.reset();
    engine.setGpuPipeline(false, {});
    REQUIRE(pumpUntil([&] { return gpu.on.has_value(); }, std::chrono::seconds(10)));
    CHECK_FALSE(*gpu.on);
    CHECK_FALSE(engine.gpuPipeline());
    const int after = frames.count;
    REQUIRE(pumpUntil([&] { return frames.count >= after + 20; }, std::chrono::seconds(5)));
    CHECK(frames.right());
    CHECK(engine.isPlaying());
    engine.shutdown();
}

TEST_CASE("Engine: shutdown while playing on the GPU is clean, and edits rebuild on it")
{
    sharedFactoryPolicy();
    Model model = makeModel();
    Engine engine(model.snapshot(), PreviewScale::Half);
    Frames frames;
    frames.attach(engine);
    GpuState gpu;
    gpu.attach(engine);
    engine.setGpuPipeline(true, {});
    REQUIRE(pumpUntil([&] { return gpu.on.has_value(); }, std::chrono::seconds(10)));
    if (!*gpu.on) {
        MESSAGE("no GPU pipeline here (" << gpu.detail << ")");
        engine.shutdown();
        return;
    }
    engine.play();
    REQUIRE(pumpUntil([&] { return frames.count >= 10; }, std::chrono::seconds(5)));
    CHECK(frames.right()); // Half: the same picture, smaller
    CHECK(frames.width == 960);

    // An edit on the GPU pipeline: a rebuild, still GPU, still right.
    model.addTrack(Track::Kind::Video, 0, "V3");
    engine.publish(model.snapshot());
    REQUIRE(engine.syncForTesting());
    const int after = frames.count;
    REQUIRE(pumpUntil([&] { return frames.count >= after + 10; }, std::chrono::seconds(5)));
    CHECK(frames.right());
    CHECK(engine.gpuPipeline());
    engine.shutdown();
}
