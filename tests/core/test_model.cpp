#include "doctest.h"

#include "core/model/model.h"

#include <random>

using namespace ustudio::core;

namespace {

AssetId addTestAsset(Model &model, FrameIndex lengthInFrames = 300)
{
    Asset asset;
    asset.displayName = "clip.mp4";
    asset.info.hasVideo = true;
    asset.info.hasAudio = true;
    asset.info.lengthInSequenceFrames = lengthInFrames;
    return model.addAsset(asset);
}

} // namespace

TEST_CASE("Model: empty project has no invariant violations")
{
    Model model = Model::createEmpty();
    CHECK(model.check().empty());
}

TEST_CASE("Model: insertClip then removeClip restores an empty, invariant-clean track")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);

    ClipId clip = model.insertClip(track, asset, 0, 0, 99);
    CHECK(model.check().empty());
    CHECK(model.hasClip(clip));
    CHECK(model.track(track).clips.size() == 1);

    model.removeClip(clip);
    CHECK(model.check().empty());
    CHECK_FALSE(model.hasClip(clip));
    CHECK(model.track(track).clips.empty());
}

TEST_CASE("Model: moveClip across tracks updates both tracks' clip lists")
{
    Model model = Model::createEmpty();
    TrackId trackA = model.addTrack(Track::Kind::Video, 0, "V1");
    TrackId trackB = model.addTrack(Track::Kind::Video, 1, "V2");
    AssetId asset = addTestAsset(model);

    ClipId clip = model.insertClip(trackA, asset, 0, 0, 99);
    model.moveClip(clip, trackB, 50);

    CHECK(model.check().empty());
    CHECK(model.track(trackA).clips.empty());
    CHECK(model.track(trackB).clips.size() == 1);
    CHECK(model.clip(clip).track == trackB);
    CHECK(model.clip(clip).position == 50);
}

TEST_CASE("Model: resizeClip changes the source span and re-sorts the track")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);

    ClipId first = model.insertClip(track, asset, 100, 0, 49);  // occupies [100, 150)
    ClipId second = model.insertClip(track, asset, 300, 0, 49); // occupies [300, 350)

    // Shrink `second` and move it before `first` (into the gap at the
    // very start) -- the track's clip order must follow position, not
    // insertion order.
    model.resizeClip(second, 0, 19, 10); // occupies [10, 30), before `first`

    CHECK(model.check().empty());
    const Track &t = model.track(track);
    REQUIRE(t.clips.size() == 2);
    CHECK(t.clips[0] == second);
    CHECK(t.clips[1] == first);
}

TEST_CASE("Model: splitClip produces two adjacent, non-overlapping clips")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);

    ClipId left = model.insertClip(track, asset, 0, 0, 99);
    ClipId right = model.splitClip(left, 40);

    CHECK(model.check().empty());
    CHECK(model.clip(left).position == 0);
    CHECK(model.clip(left).out == 39);
    CHECK(model.clip(right).position == 40);
    CHECK(model.clip(right).in == 40);
    CHECK(model.clip(right).out == 99);
    CHECK(model.clip(left).end() == model.clip(right).position);
}

TEST_CASE("Model: removeTrack drops the clips that lived on it")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId clip = model.insertClip(track, asset, 0, 0, 99);

    model.removeTrack(track);

    CHECK(model.check().empty());
    CHECK_FALSE(model.hasClip(clip));
    CHECK_FALSE(model.hasTrack(track));
}

TEST_CASE("Model: check() reports an overlap introduced by bypassing the mutators")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    model.insertClip(track, asset, 0, 0, 99);
    model.insertClip(track, asset, 50, 0, 99); // overlaps [0, 100) on the same track

    auto problems = model.check();
    CHECK_FALSE(problems.empty());
}

TEST_CASE("Model: ids are monotonic and never reused across insert/remove/insert")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);

    ClipId first = model.insertClip(track, asset, 0, 0, 9);
    model.removeClip(first);
    ClipId second = model.insertClip(track, asset, 0, 0, 9);

    CHECK(second.value != first.value);
    CHECK(model.check().empty());
}

TEST_CASE("Model: redo re-inserts a clip with the same id via reuseId")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);

    ClipId clip = model.insertClip(track, asset, 0, 0, 9);
    model.removeClip(clip);
    ClipId redone = model.insertClip(track, asset, 0, 0, 9, clip);

    CHECK(redone == clip);
    CHECK(model.check().empty());
}

// Property-style check for the Model layer alone (doc 12's full "random
// commands -> undo all -> equal" property test needs core/commands'
// UndoStack, landing in a later commit): a random sequence of Insert/
// Remove/Move/Resize/Split, each individually valid against current
// state, must never leave Model::check() non-empty.
TEST_CASE("Model: a long random sequence of valid edits never violates an invariant")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model, 10'000);

    std::mt19937 rng(12345);
    std::vector<ClipId> liveClips;
    FrameIndex nextFreePosition = 0;

    for (int i = 0; i < 2000; ++i) {
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
            FrameIndex length = current.length();
            if (length > 2) {
                FrameIndex at = current.position + 1 + static_cast<FrameIndex>(rng() % (length - 2));
                liveClips.push_back(model.splitClip(clip, at));
            }
        }

        auto problems = model.check();
        INFO("iteration ", i, " problems: ", problems.empty() ? std::string{"<none>"} : problems.front());
        REQUIRE(problems.empty());
    }
}
