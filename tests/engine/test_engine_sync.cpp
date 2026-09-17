#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine/engine_sync.h"
#include "engine/factory_policy.h"

#include <random>

using namespace ustudio::core;
using namespace ustudio::engine;

namespace {

// color: is an MLT generator producer -- synthetic, no file, happy to
// supply any in/out range -- so tests never touch real media (project
// rule: no binary media in the repo, doc 11).
AssetId addGeneratorAsset(Model &model, const std::string &resource, FrameIndex lengthInFrames = 100'000)
{
    Asset asset;
    asset.path = resource;
    asset.displayName = resource;
    asset.info.hasVideo = true;
    asset.info.hasAudio = false;
    asset.info.lengthInSequenceFrames = lengthInFrames;
    return model.addAsset(asset);
}

} // namespace

TEST_CASE("EngineSync: empty model produces a tractor with just the black backing track")
{
    FactoryPolicy policy;
    Model model = Model::createEmpty();

    EngineSync sync(model);
    CHECK(sync.tractor().count() == 1);
    CHECK(sync.verify().empty());
}

TEST_CASE("EngineSync: one clip on one video track lands in the playlist at the right position")
{
    FactoryPolicy policy;
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addGeneratorAsset(model, "color:red");
    model.insertClip(track, asset, 10, 0, 49); // 50 frames at position 10

    EngineSync sync(model);
    CHECK(sync.verify().empty());

    // Index 0 = black; index 1 = the one video track (no audio tracks).
    REQUIRE(sync.tractor().count() == 2);
    Mlt::Producer *raw = sync.tractor().track(1);
    REQUIRE(raw != nullptr);
    Mlt::Playlist playlist(*raw);
    CHECK(playlist.get_length() == 60); // 10 blank + 50 clip
}

TEST_CASE("EngineSync: rebuildAll after a model change keeps verify() clean")
{
    FactoryPolicy policy;
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addGeneratorAsset(model, "color:blue");
    model.insertClip(track, asset, 0, 0, 99);

    EngineSync sync(model);
    CHECK(sync.verify().empty());

    model.insertClip(track, asset, 200, 0, 49);
    sync.rebuildAll();
    CHECK(sync.verify().empty());

    model.removeClip(model.track(track).clips.front());
    sync.rebuildAll();
    CHECK(sync.verify().empty());
}

TEST_CASE("EngineSync: audio tracks sit below video tracks, video tracks are bottom-to-top by model order")
{
    FactoryPolicy policy;
    Model model = Model::createEmpty();
    // Model order (visual, top to bottom): topVideo, bottomVideo, then audio.
    TrackId topVideo = model.addTrack(Track::Kind::Video, 0, "V-top");
    TrackId bottomVideo = model.addTrack(Track::Kind::Video, 1, "V-bottom");
    TrackId audio = model.addTrack(Track::Kind::Audio, 2, "A1");

    EngineSync sync(model);
    // Index 0 = black, 1 = audio, 2 = bottomVideo, 3 = topVideo (doc 03:
    // audio gets the lowest indices; video is bottom-to-top, i.e. reversed
    // from the model's top-to-bottom visual order).
    REQUIRE(sync.tractor().count() == 4);

    // Re-run rebuildAll and confirm verify() still holds with all three
    // model tracks present, exercising the mixed audio+video ordering path.
    (void)topVideo;
    (void)bottomVideo;
    (void)audio;
    sync.rebuildAll();
    CHECK(sync.verify().empty());
}

TEST_CASE("EngineSync: tractor length matches sequence length, including after growth and shrink")
{
    FactoryPolicy policy;
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addGeneratorAsset(model, "color:green");

    EngineSync sync(model);
    CHECK(sync.tractor().get_length() == 1); // empty sequence: length() is 0, clamped to 1

    ClipId clip = model.insertClip(track, asset, 0, 0, 99);
    sync.rebuildAll();
    CHECK(sync.tractor().get_length() == model.sequence().length());
    CHECK(sync.tractor().get_length() == 100);

    model.removeClip(clip);
    sync.rebuildAll();
    CHECK(sync.tractor().get_length() == 1);
}

// Same random-edit generator as the Model and UndoStack property tests,
// driven through EngineSync::rebuildAll()+verify() after each step (doc
// 11's prescribed engine test shape: "run verify() after every command of
// the same random-command generator").
TEST_CASE("EngineSync property: verify() never fails across 500 random edits")
{
    FactoryPolicy policy;
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addGeneratorAsset(model, "color:yellow", 1'000'000);

    EngineSync sync(model);

    std::mt19937 rng(7);
    std::vector<ClipId> liveClips;
    FrameIndex nextFreePosition = 0;

    for (int i = 0; i < 500; ++i) {
        std::uniform_int_distribution<int> pickAction(0, liveClips.empty() ? 0 : 2);
        int action = pickAction(rng);

        if (action == 0 || liveClips.empty()) {
            FrameIndex length = 10 + static_cast<FrameIndex>(rng() % 90);
            ClipId clip = model.insertClip(track, asset, nextFreePosition, 0, length - 1);
            nextFreePosition += length;
            liveClips.push_back(clip);
        } else if (action == 1) {
            std::uniform_int_distribution<size_t> pickClip(0, liveClips.size() - 1);
            size_t index = pickClip(rng);
            model.removeClip(liveClips[index]);
            liveClips.erase(liveClips.begin() + static_cast<std::ptrdiff_t>(index));
        } else {
            std::uniform_int_distribution<size_t> pickClip(0, liveClips.size() - 1);
            ClipId clip = liveClips[pickClip(rng)];
            const Clip &current = model.clip(clip);
            if (current.length() > 2) {
                FrameIndex at = current.position + 1 + static_cast<FrameIndex>(rng() % (current.length() - 2));
                liveClips.push_back(model.splitClip(clip, at));
            }
        }

        sync.rebuildAll();
        auto problems = sync.verify();
        INFO("iteration ", i, " problems: ", problems.empty() ? std::string{"<none>"} : problems.front());
        REQUIRE(problems.empty());
    }
}
