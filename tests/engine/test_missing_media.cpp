// M4 B, doc 12's acceptance: move a media file away and reopen the project
// -> its clips play the missing-media placeholder (the rest keeps playing);
// relink restores everything with no other model change.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/commands/primitives.h"
#include "core/commands/undo_stack.h"
#include "core/media/fingerprint.h"
#include "core/media/missing_media.h"
#include "core/media/utf8_path.h"
#include "core/xml/reader.h"
#include "core/xml/writer.h"
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

FactoryPolicy &sharedFactoryPolicy()
{
    static FactoryPolicy policy;
    return policy;
}

struct Pixel
{
    int r, g, b;
};

Pixel pixelAt(EngineSync &sync, int position)
{
    Mlt::Tractor &tractor = sync.tractor();
    tractor.seek(position);
    std::unique_ptr<Mlt::Frame> frame(tractor.get_frame());
    mlt_image_format format = mlt_image_rgb;
    int w = 64, h = 36;
    const uint8_t *image = frame->get_image(format, w, h);
    const size_t i = (static_cast<size_t>(h / 2) * static_cast<size_t>(w) + static_cast<size_t>(w / 2)) * 3;
    return {image[i], image[i + 1], image[i + 2]};
}

// A tiny green H.264 file, generated (no binary media in the repo).
void renderGreen(const fs::path &path)
{
    Model model = Model::createEmpty();
    EngineSync sync(model);
    Mlt::Producer producer(sync.profile(), "color:#00c000");
    producer.set_in_and_out(0, 29);
    std::unique_ptr<Mlt::Profile> profile(producer.profile());
    Mlt::Consumer consumer(*profile, "avformat", utf8String(path).c_str());
    consumer.set("vcodec", h264Encoder().c_str());
    consumer.connect(producer);
    consumer.run();
    consumer.stop(); // joins the render-ahead thread (notes/render.md)
}

} // namespace

TEST_CASE("missing media: placeholders on reopen, playback continues, relink restores it exactly")
{
    sharedFactoryPolicy();
    const fs::path dir =
        fs::temp_directory_path() / ("ustudio-missing-" + std::to_string(platform::currentProcessId()));
    fs::create_directories(dir);
    const fs::path media = dir / pathFromUtf8("grün clip.mp4");
    const fs::path moved = dir / pathFromUtf8("moved/grün clip.mp4");
    const fs::path project = dir / "project.ustudio";
    renderGreen(media);

    // A green clip, then a generator clip that never goes missing.
    Model original = Model::createEmpty();
    TrackId track = original.addTrack(Track::Kind::Video, 0, "V1");
    Asset asset;
    asset.path = utf8String(media);
    asset.displayName = "grün clip.mp4";
    asset.info.hasVideo = true;
    asset.info.lengthInSequenceFrames = 30;
    asset.status = Asset::Status::Ready;
    asset.fileFingerprint = fileFingerprint(asset.path);
    AssetId green = original.addAsset(asset);
    Asset generator;
    generator.path = "color:blue";
    generator.info.hasVideo = true;
    generator.info.lengthInSequenceFrames = 1000;
    AssetId blue = original.addAsset(generator);
    original.insertClip(track, green, 0, 0, 29);
    original.insertClip(track, blue, 30, 0, 29);
    REQUIRE(saveProject(original, utf8String(project)).empty());

    fs::create_directories(moved.parent_path());
    fs::rename(media, moved);

    auto loaded = loadProject(utf8String(project));
    REQUIRE(loaded.has_value());
    Model model = std::move(*loaded);
    CHECK(markMissingMedia(model) == std::vector<AssetId>{green});
    CHECK(model.asset(green).status == Asset::Status::Missing);
    CHECK(model.asset(blue).status != Asset::Status::Missing); // a generator is never "missing"

    EngineSync sync(model);
    CHECK(sync.verify().empty());
    const Pixel placeholder = pixelAt(sync, 10);
    CHECK(placeholder.r > 90);
    CHECK(placeholder.g < 60); // dark red, not black and not green
    const Pixel after = pixelAt(sync, 40);
    CHECK(after.b > 150); // the rest of the timeline still plays

    // Saved while missing: stored as Ready (the next open checks again).
    const fs::path resaved = dir / "resaved.ustudio";
    REQUIRE(saveProject(model, utf8String(resaved)).empty());
    CHECK(loadProject(utf8String(resaved))->asset(green).status == Asset::Status::Ready);

    // Relink: one command; the engine reopens the file.
    UndoStack undo(model);
    const Model beforeRelink = model;
    REQUIRE(undo.execute(std::make_unique<RelinkAsset>(green, utf8String(moved), fileFingerprint(utf8String(moved)))));
    sync.setProject(model.snapshot());
    const Pixel relinked = pixelAt(sync, 10);
    CHECK(relinked.g > 120);
    CHECK(relinked.r < 60);

    // Nothing but the path changed: the original project, pointed at the
    // new place, is exactly what we have now.
    Model expected = original;
    expected.setAssetSource(green, utf8String(moved), original.asset(green).fileFingerprint, Asset::Status::Ready);
    Model reread = std::move(*loadProject(utf8String(project)));
    CHECK(model.project().sequences == reread.project().sequences);
    CHECK(model.asset(green) == expected.asset(green));
    CHECK(model.asset(blue) == reread.asset(blue));

    // Undo puts it back as missing.
    REQUIRE(undo.undo());
    CHECK(model == beforeRelink);
    sync.setProject(model.snapshot());
    CHECK(pixelAt(sync, 10).g < 60);

    fs::remove_all(dir);
}
