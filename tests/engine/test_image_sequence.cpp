// M4 E: an image sequence plays one image per frame, for its counted length.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/media/image_sequence.h"
#include "core/media/missing_media.h"
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

// Five red frames then five blue, as frame_0001.png ... frame_0010.png
// (MLT's avformat consumer with FFmpeg's image2 muxer).
fs::path makeSequence(const fs::path &dir)
{
    fs::create_directories(dir);
    Profile small;
    small.width = 320;
    small.height = 180;
    Model model = Model::createEmpty(small);
    EngineSync sync(model);
    Mlt::Profile &p = sync.profile();
    Mlt::Playlist playlist(p);
    Mlt::Producer red(p, "color:#ff0000"), blue(p, "color:#0000ff");
    playlist.append(red, 0, 4);
    playlist.append(blue, 0, 4);
    Mlt::Consumer consumer(p, "avformat", utf8String(dir / "frame_%04d.png").c_str());
    consumer.set("f", "image2");
    consumer.set("vcodec", "png");
    consumer.set("start_number", 1);
    consumer.set("an", 1);
    consumer.set("real_time", -1);
    consumer.connect(playlist);
    consumer.run();
    return dir;
}

bool blueAt(EngineSync &sync, int position)
{
    sync.tractor().seek(position);
    std::unique_ptr<Mlt::Frame> frame(sync.tractor().get_frame());
    mlt_image_format format = mlt_image_rgb;
    int w = 320, h = 180;
    const uint8_t *px = frame->get_image(format, w, h) + (90 * 320 + 160) * 3;
    return px[2] > 150 && px[0] < 90;
}

} // namespace

TEST_CASE("image sequence: one image per frame, its counted length, missing when its files go")
{
    static FactoryPolicy policy;
    const fs::path dir =
        makeSequence(fs::temp_directory_path() / ("ustudio-seq-" + std::to_string(platform::currentProcessId())));
    REQUIRE(fs::exists(dir / "frame_0010.png"));
    auto sequence = findImageSequence(utf8String(dir / "frame_0004.png"));
    REQUIRE(sequence);
    CHECK(sequence->begin == 1);
    CHECK(sequence->count == 10);
    const EngineSync::ProbedMedia first =
        EngineSync::probeMediaFile(Profile{}, imageSequenceFile(sequence->pattern, 1));
    CHECK(first.isStillImage);
    CHECK(first.width == 320); // the import command needs the size
    CHECK(first.height == 180);

    Profile small;
    small.width = 320;
    small.height = 180;
    Model model = Model::createEmpty(small);
    const TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset asset;
    asset.path = sequence->pattern;
    asset.status = Asset::Status::Ready;
    asset.info.hasVideo = true;
    asset.info.isImageSequence = true;
    asset.info.sequenceBegin = sequence->begin;
    asset.info.width = 320;
    asset.info.height = 180;
    asset.info.lengthInSequenceFrames = sequence->count;
    const AssetId id = model.addAsset(asset);
    CHECK_FALSE(model.asset(id).info.isBoundless()); // a real length
    model.insertClip(track, id, 0, 0, 9);
    {
        EngineSync sync(model);
        CHECK(sync.verify().empty());
        CHECK(sync.tractor().get_length() == 10);
        CHECK_FALSE(blueAt(sync, 2)); // frame_0003: red
        CHECK(blueAt(sync, 7));       // frame_0008: blue
    }
    CHECK(markMissingMedia(model).empty());
    fs::remove(dir / "frame_0001.png"); // its first file stands for it
    CHECK(markMissingMedia(model) == std::vector<AssetId>{id});
    fs::remove_all(dir);
}
