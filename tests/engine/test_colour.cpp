// BT.709 sources keep their colours in preview and export: the graph's
// black background (producer_colour) used to tag every composited frame
// BT.601, so red came out 233 and cyan's red 22 (2026-09-27).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/commands/primitives.h"
#include "core/commands/undo_stack.h"
#include "core/media/utf8_path.h"
#include "core/xml/reader.h"
#include "core/xml/writer.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"
#include "platform/process.h"

#include <cstdlib>
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

TEST_CASE("colour: the project background survives save and load, and plays and exports as that colour")
{
    static FactoryPolicy policy;
    const fs::path dir = fs::temp_directory_path() / ("ustudio-bg-" + std::to_string(platform::currentProcessId()));
    fs::create_directories(dir);
    auto near = [](const Rgb &px, uint32_t rgb, int tolerance) {
        CAPTURE(px.r);
        CAPTURE(px.g);
        CAPTURE(px.b);
        CHECK(std::abs(px.r - static_cast<int>(rgb >> 16 & 0xff)) <= tolerance);
        CHECK(std::abs(px.g - static_cast<int>(rgb >> 8 & 0xff)) <= tolerance);
        CHECK(std::abs(px.b - static_cast<int>(rgb & 0xff)) <= tolerance);
    };

    // A white clip at 30-59: frames 0-29 are the background alone.
    Model model = Model::createEmpty();
    const TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset white;
    white.path = "color:white";
    white.info.hasVideo = true;
    white.info.lengthInSequenceFrames = 10'000;
    model.insertClip(track, model.addAsset(white), 30, 0, 29);
    UndoStack undo(model);
    REQUIRE(undo.execute(std::make_unique<SetSequenceBackground>(0x3366cc)));
    CHECK_FALSE(undo.execute(std::make_unique<SetSequenceBackground>(0x3366cc))); // already that colour
    CHECK(undo.undoLabel() == "Set background colour");

    const fs::path project = dir / "background.ustudio";
    REQUIRE(saveProject(model, utf8String(project)).empty());
    auto loaded = loadProject(utf8String(project));
    REQUIRE(loaded.has_value());
    CHECK(loaded->sequence().background == 0x3366cc);

    {
        EngineSync sync(*loaded);
        INFO("editor");
        near(pixelAt(sync.tractor(), 5, 960, 540), 0x3366cc, 2);
        near(pixelAt(sync.tractor(), 45, 960, 540), 0xffffff, 2); // the clip covers it
        Mlt::Producer melt(sync.profile(), "xml", utf8String(project).c_str());
        REQUIRE(melt.is_valid());
        INFO("melt");
        near(pixelAt(melt, 5, 960, 540), 0x3366cc, 2); // melt plays the saved file the same

        // A new colour rebuilds the graph with it; undo restores it.
        undo.undo();
        CHECK(model.sequence().background == Sequence::kDefaultBackground);
        model.setSequenceBackground(0x808080); // a grey: the YUV path
        sync.setProject(model.snapshot());
        INFO("grey");
        near(pixelAt(sync.tractor(), 5, 960, 540), 0x808080, 2);
    }

    const fs::path rendered = dir / "background.mp4";
    std::string error;
    REQUIRE(renderProject(*loaded, utf8String(rendered), error));
    Mlt::Profile profile("atsc_1080p_30");
    Mlt::Producer decoded(profile, utf8String(rendered).c_str());
    REQUIRE(decoded.is_valid());
    near(pixelAt(decoded, 5, 960, 540), 0x3366cc, 6); // lossy H.264
    fs::remove_all(dir);
}
