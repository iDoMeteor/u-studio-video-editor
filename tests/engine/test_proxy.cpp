// M4 C: proxies. renderProxy() makes a smaller, constant-rate stand-in with
// the source's duration; EngineSync plays it only when asked, falls back to
// the original when its file is gone, and renders never use it.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/media/utf8_path.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"
#include "engine/proxy.h"
#include "platform/process.h"

#include <atomic>
#include <filesystem>
#include <memory>

using namespace ustudio;
using namespace ustudio::core;
using namespace ustudio::engine;
namespace fs = std::filesystem;

namespace {

FactoryPolicy &sharedFactoryPolicy()
{
    static FactoryPolicy policy;
    return policy;
}

fs::path scratch()
{
    static const fs::path dir =
        fs::temp_directory_path() / ("ustudio-proxy-" + std::to_string(platform::currentProcessId()));
    fs::create_directories(dir);
    return dir;
}

// `frames` of a solid colour at width x height, 30 fps.
fs::path generate(const std::string &name, const char *colour, int width, int height, int frames)
{
    const fs::path path = scratch() / name;
    Profile profile;
    profile.width = width;
    profile.height = height;
    profile.fps = {30, 1};
    Model model = Model::createEmpty(profile);
    EngineSync sync(model);
    Mlt::Producer producer(sync.profile(), colour);
    producer.set_in_and_out(0, frames - 1);
    std::unique_ptr<Mlt::Profile> consumerProfile(producer.profile());
    Mlt::Consumer consumer(*consumerProfile, "avformat", utf8String(path).c_str());
    consumer.set("vcodec", h264Encoder().c_str());
    consumer.connect(producer);
    consumer.run();
    consumer.stop(); // joins the render-ahead thread (notes/render.md)
    return path;
}

struct Rgb
{
    int r, g, b;
};

// The pixel at (x, y) of a 192x108 frame.
Rgb pixelAt(EngineSync &sync, int position, int x = 96, int y = 54)
{
    Mlt::Tractor &tractor = sync.tractor();
    tractor.seek(position);
    std::unique_ptr<Mlt::Frame> frame(tractor.get_frame());
    mlt_image_format format = mlt_image_rgb;
    int w = 192, h = 108;
    const uint8_t *image = frame->get_image(format, w, h);
    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3;
    return {image[i], image[i + 1], image[i + 2]};
}

Model oneClip(const fs::path &source, const std::string &proxy)
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset asset;
    asset.path = utf8String(source);
    asset.proxyPath = proxy;
    asset.info.hasVideo = true;
    asset.info.lengthInSequenceFrames = 20;
    AssetId id = model.addAsset(asset);
    model.insertClip(track, id, 0, 0, 19);
    return model;
}

} // namespace

TEST_CASE("renderProxy: a 2160p source becomes a 540p proxy of the same duration")
{
    sharedFactoryPolicy();
    const fs::path source = generate("source-4k.mp4", "color:#00c000", 3840, 2160, 20);
    const fs::path proxy = scratch() / "proxy.mp4";
    ProxyRequest request;
    request.source = utf8String(source);
    request.output = utf8String(proxy);
    request.height = 540;
    request.fps = {30, 1};
    int progressCalls = 0;
    std::string error;
    REQUIRE(renderProxy(request, error, [&](int, int) { ++progressCalls; }));
    CHECK(error.empty());
    CHECK_FALSE(fs::exists(utf8String(proxy) + ".part"));

    Profile probeProfile;
    probeProfile.fps = {30, 1};
    const EngineSync::ProbedMedia probed = EngineSync::probeMediaFile(probeProfile, utf8String(proxy));
    CHECK(probed.height == 540);
    CHECK(probed.width == 960);
    CHECK(probed.length == 20); // the source's duration, frame for frame at 30 fps

    // Cancelled: no proxy, no .part left behind.
    const fs::path cancelled = scratch() / "cancelled.mp4";
    request.output = utf8String(cancelled);
    std::atomic<bool> cancel{true};
    CHECK_FALSE(renderProxy(request, error, {}, &cancel));
    CHECK_FALSE(fs::exists(cancelled));
    CHECK_FALSE(fs::exists(utf8String(cancelled) + ".part"));

    // A still image needs none.
    request.source = "color:red";
    CHECK_FALSE(renderProxy(request, error));
}

TEST_CASE("proxies: the toggle swaps what plays, a gone proxy falls back, and a render uses the original")
{
    sharedFactoryPolicy();
    // The "proxy" is a different colour on purpose, to see which one plays.
    const fs::path source = generate("green.mp4", "color:#00c000", 1280, 720, 20);
    const fs::path proxy = generate("blue-proxy.mp4", "color:#0000c0", 640, 360, 20);
    Model model = oneClip(source, utf8String(proxy));
    const auto before = model.snapshot();

    EngineSync sync(model);
    CHECK(pixelAt(sync, 5).g > 120); // off by default: the original
    sync.setUseProxies(true);
    const Rgb proxied = pixelAt(sync, 5);
    CHECK(proxied.b > 120);
    CHECK(proxied.g < 60);
    CHECK(sync.verify().empty());        // a proxied clip's resource isn't its asset's path, by design
    CHECK(*model.snapshot() == *before); // the toggle is view state: the model is untouched

    // A render, whatever the toggle, uses the original.
    const fs::path rendered = scratch() / "render.mp4";
    std::string error;
    REQUIRE(renderProject(model, utf8String(rendered), error));
    Model renderedModel = oneClip(rendered, "");
    EngineSync check(renderedModel);
    CHECK(pixelAt(check, 5).g > 120);

    // The cache was cleared: the original plays, quietly.
    sync.setUseProxies(false);
    fs::remove(proxy);
    sync.setUseProxies(true);
    CHECK(pixelAt(sync, 5).g > 120);
    CHECK(sync.verify().empty());
    fs::remove_all(scratch());
}
