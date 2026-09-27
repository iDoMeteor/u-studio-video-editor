// BT.709 sources keep their colours in preview and export: the graph's
// black background (producer_colour) used to tag every composited frame
// BT.601, so red came out 233 and cyan's red 22 (2026-09-27).

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

namespace {

struct Rgb
{
    int r, g, b;
};

Rgb pixelAt(Mlt::Producer &producer, int position, int x, int y)
{
    producer.seek(position);
    std::unique_ptr<Mlt::Frame> frame(producer.get_frame());
    mlt_image_format format = mlt_image_rgba;
    int w = 1920, h = 1080;
    const uint8_t *px = frame->get_image(format, w, h) + (static_cast<size_t>(y) * 1920 + x) * 4;
    return {px[0], px[1], px[2]};
}

// Red on the left half, cyan on the right, BT.709 H.264 at 1080p.
fs::path makeBars(const fs::path &path)
{
    Model model = Model::createEmpty(); // 1080p30, colorspace 709
    EngineSync sync(model);
    Mlt::Profile &p = sync.profile();
    Mlt::Tractor tractor(p);
    Mlt::Producer red(p, "color:#ff0000"), cyan(p, "color:#00ffff");
    // RGBA, so the encoder's conversion to YUV uses the profile's BT.709
    // (a colour producer's own YUV is tagged 601: the bug under test).
    red.set("mlt_image_format", "rgba");
    cyan.set("mlt_image_format", "rgba");
    red.set_in_and_out(0, 29);
    cyan.set_in_and_out(0, 29);
    tractor.set_track(red, 0);
    tractor.set_track(cyan, 1);
    Mlt::Transition half(p, "composite");
    half.set("geometry", "960/0:960x1080");
    half.set("distort", 1);
    std::unique_ptr<Mlt::Field> field(tractor.field());
    field->plant_transition(half, 0, 1);
    Mlt::Consumer consumer(p, "avformat", utf8String(path).c_str());
    consumer.set("vcodec", h264Encoder().c_str());
    consumer.set("crf", 1);
    consumer.set("an", 1);
    consumer.set("real_time", -1);
    consumer.connect(tractor);
    consumer.run();
    return path;
}

void checkColours(const Rgb &red, const Rgb &cyan)
{
    CAPTURE(red.r);
    CAPTURE(cyan.r);
    CHECK(red.r >= 248); // 233 with the 601 matrix
    CHECK(red.g <= 8);
    CHECK(cyan.r <= 8); // 22 with the 601 matrix
    CHECK(cyan.g >= 248);
    CHECK(cyan.b >= 248);
}

} // namespace

TEST_CASE("colour: a BT.709 source keeps its colours through the graph and in a render")
{
    static FactoryPolicy policy;
    const fs::path dir = fs::temp_directory_path() / ("ustudio-colour-" + std::to_string(platform::currentProcessId()));
    fs::create_directories(dir);
    const fs::path bars = makeBars(dir / "bars709.mp4");
    {
        Mlt::Profile profile("atsc_1080p_30");
        Mlt::Producer alone(profile, utf8String(bars).c_str());
        checkColours(pixelAt(alone, 10, 480, 540), pixelAt(alone, 10, 1440, 540)); // the source is right
    }

    Model model = Model::createEmpty();
    const TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset asset;
    asset.path = utf8String(bars);
    asset.status = Asset::Status::Ready;
    asset.info.hasVideo = true;
    asset.info.width = 1920;
    asset.info.height = 1080;
    asset.info.lengthInSequenceFrames = 30;
    model.insertClip(track, model.addAsset(asset), 0, 0, 29);
    {
        EngineSync sync(model);
        checkColours(pixelAt(sync.tractor(), 10, 480, 540), pixelAt(sync.tractor(), 10, 1440, 540));
    }

    const fs::path rendered = dir / "render.mp4";
    std::string error;
    REQUIRE(renderProject(model, utf8String(rendered), error));
    Mlt::Profile profile("atsc_1080p_30");
    Mlt::Producer decoded(profile, utf8String(rendered).c_str());
    REQUIRE(decoded.is_valid());
    checkColours(pixelAt(decoded, 10, 480, 540), pixelAt(decoded, 10, 1440, 540));
    fs::remove_all(dir);
}
