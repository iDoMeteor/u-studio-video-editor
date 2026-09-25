// Every picture fills the frame: composite's fill is 0 unless set, though
// its YAML says 1 (MLT 7.40), and a 640x360 source in a 1080p project drew
// at its own size in the top-left third.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/media/utf8_path.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"
#include "platform/process.h"

#include <filesystem>
#include <memory>

using namespace ustudio;
using namespace ustudio::core;
using namespace ustudio::engine;
namespace fs = std::filesystem;

TEST_CASE("a picture smaller than the project fills the frame")
{
    static FactoryPolicy policy;
    const fs::path path = fs::temp_directory_path() /
                          ("ustudio-fill-" + std::to_string(platform::currentProcessId()) + ".mp4");
    {
        Profile small;
        small.width = 640;
        small.height = 360;
        Model model = Model::createEmpty(small);
        EngineSync sync(model);
        Mlt::Producer producer(sync.profile(), "color:#0000c0");
        producer.set_in_and_out(0, 19);
        std::unique_ptr<Mlt::Profile> profile(producer.profile());
        Mlt::Consumer consumer(*profile, "avformat", utf8String(path).c_str());
        consumer.set("vcodec", h264Encoder().c_str());
        consumer.connect(producer);
        consumer.run();
    }

    Model model = Model::createEmpty(); // 1920x1080
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset asset;
    asset.path = utf8String(path);
    asset.info.hasVideo = true;
    asset.info.lengthInSequenceFrames = 20;
    model.insertClip(track, model.addAsset(asset), 0, 0, 19);
    EngineSync sync(model);
    Mlt::Tractor &tractor = sync.tractor();
    tractor.seek(5);
    std::unique_ptr<Mlt::Frame> frame(tractor.get_frame());
    mlt_image_format format = mlt_image_rgb;
    int w = 192, h = 108;
    const uint8_t *image = frame->get_image(format, w, h);
    for (auto [x, y] : {std::pair{5, 5}, std::pair{96, 54}, std::pair{186, 102}}) {
        INFO("at " << x << "," << y);
        CHECK(image[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3 + 2] > 120);
    }
    fs::remove(path);
}
