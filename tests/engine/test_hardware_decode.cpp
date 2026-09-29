// ADR-019 G1: hardware decode is per live master producer, worker producers
// never take the GPU chain, and a device that can't decode falls back to
// software. meson runs this with MLT_AVFORMAT_HWACCEL_DEVICE pointing at a
// device that doesn't exist, so the fallback is what's exercised on every
// machine; docs/developer/notes/gpu.md has the VAAPI repro.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/media/utf8_path.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"
#include "engine/producer_open.h"
#include "platform/process.h"

#include <filesystem>
#include <memory>
#include <string>

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
        fs::temp_directory_path() / ("ustudio-hwdecode-" + std::to_string(platform::currentProcessId()));
    fs::create_directories(dir);
    return dir;
}

// 20 frames of green, 320x180 at 30 fps, H.264.
fs::path generateGreen()
{
    const fs::path path = scratch() / "green.mp4";
    if (fs::exists(path))
        return path;
    Profile profile;
    profile.width = 320;
    profile.height = 180;
    profile.fps = {30, 1};
    Model model = Model::createEmpty(profile);
    EngineSync sync(model);
    Mlt::Producer producer(sync.profile(), "color:#00c000");
    producer.set_in_and_out(0, 19);
    std::unique_ptr<Mlt::Profile> consumerProfile(producer.profile());
    Mlt::Consumer consumer(*consumerProfile, "avformat", utf8String(path).c_str());
    consumer.set("vcodec", h264Encoder().c_str());
    consumer.connect(producer);
    consumer.run();
    consumer.stop(); // joins the render-ahead thread (notes/render.md)
    return path;
}

bool hasMovitFilter(Mlt::Producer &producer)
{
    for (int i = 0; i < producer.filter_count(); ++i) {
        std::unique_ptr<Mlt::Filter> filter(producer.filter(i));
        const char *service = filter->get("mlt_service");
        if (service && std::string(service).starts_with("movit."))
            return true;
    }
    return false;
}

// The resource of the first clip on any track whose resource names `path`.
std::string clipResource(EngineSync &sync, const std::string &path)
{
    Mlt::Tractor &tractor = sync.tractor();
    for (int t = 0; t < tractor.count(); ++t) {
        std::unique_ptr<Mlt::Producer> track(tractor.track(t));
        Mlt::Playlist playlist(*track);
        for (int c = 0; playlist.is_valid() && c < playlist.count(); ++c) {
            std::unique_ptr<Mlt::ClipInfo> info(playlist.clip_info(c));
            if (info && info->resource && std::string(info->resource).starts_with(path))
                return info->resource;
        }
    }
    return {};
}

int greenAt(EngineSync &sync, int position)
{
    Mlt::Tractor &tractor = sync.tractor();
    tractor.seek(position);
    std::unique_ptr<Mlt::Frame> frame(tractor.get_frame());
    mlt_image_format format = mlt_image_rgb;
    int w = 320, h = 180;
    const uint8_t *image = frame->get_image(format, w, h);
    return image ? image[(90 * 320 + 160) * 3 + 1] : -1;
}

} // namespace

TEST_CASE("the hardware-decode query is appended and stripped exactly")
{
    CHECK(withHardwareDecode("/m/a.mp4", "") == "/m/a.mp4");
    CHECK(withHardwareDecode("/m/a.mp4", "vaapi") == "/m/a.mp4\\?hwaccel=vaapi");
    CHECK(withoutHardwareDecode("/m/a.mp4\\?hwaccel=vaapi") == "/m/a.mp4");
    CHECK(withoutHardwareDecode("/m/what?.mp4") == "/m/what?.mp4"); // a literal '?' is part of a name
}

TEST_CASE("worker producers keep the CPU chain once a glsl.manager exists")
{
    sharedFactoryPolicy();
    const std::string green = utf8String(generateGreen());
    Mlt::Profile profile;
    {
        Mlt::Filter manager(profile, "glsl.manager");
        if (!manager.is_valid()) {
            MESSAGE("no movit module here; nothing to check");
            return;
        }
        // The switch this guards against: the default loader goes GPU.
        std::unique_ptr<Mlt::Producer> live = openProducer(profile, green, ProducerUse::GpuGraph);
        CHECK(hasMovitFilter(*live));
        std::unique_ptr<Mlt::Producer> worker = openProducer(profile, green, ProducerUse::Worker);
        REQUIRE(worker->is_valid());
        CHECK_FALSE(hasMovitFilter(*worker));
    }
    // Destroying the manager doesn't switch MLT back; clearing the global does.
    mlt_properties_set_data(mlt_global_properties(), "glslManager", nullptr, 0, nullptr, nullptr);
    std::unique_ptr<Mlt::Producer> after = openProducer(profile, green, ProducerUse::GpuGraph);
    CHECK_FALSE(hasMovitFilter(*after));
}

TEST_CASE("hardware decode reaches live masters only and falls back when the device fails")
{
    sharedFactoryPolicy();
    const std::string green = utf8String(generateGreen());
    Profile profile;
    profile.width = 320;
    profile.height = 180;
    profile.fps = {30, 1};
    Model model = Model::createEmpty(profile);
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset asset;
    asset.path = green;
    asset.info.hasVideo = true;
    asset.info.lengthInSequenceFrames = 20;
    AssetId id = model.addAsset(asset);
    model.insertClip(track, id, 0, 0, 19);

    EngineSync sync(model);
    CHECK(clipResource(sync, green) == green);

    sync.setHardwareDecode("vaapi");
    CHECK(clipResource(sync, green) == green + "\\?hwaccel=vaapi");
    CHECK(greenAt(sync, 5) > 150); // decoded in software: the device doesn't exist
    CHECK(sync.verify().empty());  // the query isn't a resource mismatch

    sync.setHardwareDecode("");
    CHECK(clipResource(sync, green) == green);
    CHECK(greenAt(sync, 5) > 150);
    fs::remove_all(scratch());
}
