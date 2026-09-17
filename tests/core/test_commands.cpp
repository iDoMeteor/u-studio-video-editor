#include "doctest.h"

#include "core/commands/primitives.h"
#include "core/commands/transaction.h"
#include "core/commands/undo_stack.h"
#include "core/model/model.h"

#include <random>

using namespace ustudio::core;

namespace {

AssetId addTestAsset(Model &model, FrameIndex lengthInFrames = 100'000)
{
    Asset asset;
    asset.displayName = "clip.mp4";
    asset.info.hasVideo = true;
    asset.info.hasAudio = true;
    asset.info.lengthInSequenceFrames = lengthInFrames;
    return model.addAsset(asset);
}

// Project::nextId is a monotonic counter that is never rolled back, even
// by undo (doc 03: ids are "never reused within a document" -- an id
// briefly allocated then undone must not be handed out again, so a later,
// unrelated command can't collide with a possible future redo). That
// means a fresh apply-then-revert of any id-allocating command (insert,
// split, add track/asset) leaves nextId advanced even though every
// observable field is back to where it started. Tests that exercise such
// a command compare state ignoring that one allocator field.
bool equalIgnoringIdAllocator(const Model &a, const Model &b)
{
    Project pa = a.project();
    Project pb = b.project();
    pa.nextId = pb.nextId = 0;
    return pa == pb;
}

} // namespace

TEST_CASE("InsertClip: apply then revert restores an equal model")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    Model before = model;

    InsertClip cmd(track, asset, 0, 0, 99);
    REQUIRE(cmd.apply(model));
    CHECK(model.hasClip(cmd.clipId()));
    CHECK_FALSE(model == before);

    cmd.revert(model);
    CHECK(equalIgnoringIdAllocator(model, before));
}

TEST_CASE("InsertClip refuses an overlapping range and leaves the model untouched")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    model.insertClip(track, asset, 0, 0, 99);
    Model before = model;

    InsertClip cmd(track, asset, 50, 0, 99); // overlaps [0, 100)
    CHECK_FALSE(cmd.apply(model));
    CHECK(model == before);
}

TEST_CASE("RemoveClip: revert restores the exact original clip, not just its position")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId clip = model.insertClip(track, asset, 0, 0, 99);
    Model before = model;

    RemoveClip cmd(clip);
    REQUIRE(cmd.apply(model));
    CHECK_FALSE(model.hasClip(clip));

    cmd.revert(model);
    CHECK(model == before);
}

TEST_CASE("MoveClip: apply then revert restores an equal model")
{
    Model model = Model::createEmpty();
    TrackId trackA = model.addTrack(Track::Kind::Video, 0, "V1");
    TrackId trackB = model.addTrack(Track::Kind::Video, 1, "V2");
    AssetId asset = addTestAsset(model);
    ClipId clip = model.insertClip(trackA, asset, 0, 0, 99);
    Model before = model;

    MoveClip cmd(clip, trackB, 200);
    REQUIRE(cmd.apply(model));
    CHECK(model.clip(clip).track == trackB);

    cmd.revert(model);
    CHECK(model == before);
}

TEST_CASE("ResizeClip: apply then revert restores an equal model")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId clip = model.insertClip(track, asset, 100, 0, 99);
    Model before = model;

    ResizeClip cmd(clip, 10, 49, 100);
    REQUIRE(cmd.apply(model));
    CHECK(model.clip(clip).length() == 40);

    cmd.revert(model);
    CHECK(model == before);
}

TEST_CASE("SplitClip: apply then revert restores an equal model, including the removed right half")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId clip = model.insertClip(track, asset, 0, 0, 99);
    Model before = model;

    SplitClip cmd(clip, 40);
    REQUIRE(cmd.apply(model));
    CHECK(model.hasClip(cmd.rightId()));
    CHECK(model.clip(clip).out == 39);

    cmd.revert(model);
    CHECK_FALSE(model.hasClip(cmd.rightId()));
    CHECK(equalIgnoringIdAllocator(model, before));
}

TEST_CASE("AddTrack / RemoveTrack: revert restores the track's clips too")
{
    Model model = Model::createEmpty();
    Model before = model;

    AddTrack addCmd(Track::Kind::Video, 0, "V1");
    REQUIRE(addCmd.apply(model));
    AssetId asset = addTestAsset(model);
    ClipId clip = model.insertClip(addCmd.trackId(), asset, 0, 0, 99);
    (void)clip;

    Model beforeRemoval = model;
    RemoveTrack removeCmd(addCmd.trackId());
    REQUIRE(removeCmd.apply(model));
    CHECK_FALSE(model.hasTrack(addCmd.trackId()));

    removeCmd.revert(model);
    CHECK(model == beforeRemoval);
}

TEST_CASE("SetTrackFlags: revert restores old flags")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Model before = model;

    SetTrackFlags cmd(track, true, true, false);
    REQUIRE(cmd.apply(model));
    CHECK(model.track(track).muted);

    cmd.revert(model);
    CHECK(model == before);
}

TEST_CASE("Transaction: a failing command rolls back everything applied before it")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    Model before = model;

    Transaction tx(model);
    CHECK(tx.run(std::make_unique<InsertClip>(track, asset, 0, 0, 99)));
    // Overlaps the clip just inserted -- must fail and unwind the whole transaction.
    CHECK_FALSE(tx.run(std::make_unique<InsertClip>(track, asset, 50, 0, 99)));

    CHECK(equalIgnoringIdAllocator(model, before));
}

TEST_CASE("Transaction: commit keeps every applied command's effect")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);

    Transaction tx(model);
    REQUIRE(tx.run(std::make_unique<InsertClip>(track, asset, 0, 0, 99)));
    REQUIRE(tx.run(std::make_unique<InsertClip>(track, asset, 200, 0, 99)));
    tx.commit();

    CHECK(model.track(track).clips.size() == 2);
}

TEST_CASE("UndoStack: execute/undo/redo and labels")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    UndoStack undoStack(model);

    CHECK_FALSE(undoStack.canUndo());
    REQUIRE(undoStack.execute(std::make_unique<InsertClip>(track, asset, 0, 0, 99)));
    CHECK(undoStack.canUndo());
    CHECK_FALSE(undoStack.canRedo());
    CHECK(undoStack.undoLabel() == "Insert clip");
    CHECK(model.track(track).clips.size() == 1);

    CHECK(undoStack.undo());
    CHECK(model.track(track).clips.empty());
    CHECK(undoStack.canRedo());

    CHECK(undoStack.redo());
    CHECK(model.track(track).clips.size() == 1);
    CHECK_FALSE(undoStack.canRedo());
}

TEST_CASE("UndoStack: a refused command changes nothing and isn't pushed")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    UndoStack undoStack(model);

    REQUIRE(undoStack.execute(std::make_unique<InsertClip>(track, asset, 0, 0, 99)));
    CHECK_FALSE(undoStack.execute(std::make_unique<InsertClip>(track, asset, 50, 0, 99))); // overlaps
    CHECK(model.track(track).clips.size() == 1);
    CHECK(undoStack.undoLabel() == "Insert clip"); // still the first one; nothing new was pushed
}

TEST_CASE("UndoStack: executing a new command after undo clears the redo stack")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    UndoStack undoStack(model);

    REQUIRE(undoStack.execute(std::make_unique<InsertClip>(track, asset, 0, 0, 99)));
    CHECK(undoStack.undo());
    CHECK(undoStack.canRedo());

    REQUIRE(undoStack.execute(std::make_unique<InsertClip>(track, asset, 200, 0, 99)));
    CHECK_FALSE(undoStack.canRedo());
}

TEST_CASE("UndoStack: isClean tracks the save point across undo/redo")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    UndoStack undoStack(model);

    CHECK(undoStack.isClean()); // nothing executed yet: clean by definition
    undoStack.setCleanPoint();

    REQUIRE(undoStack.execute(std::make_unique<InsertClip>(track, asset, 0, 0, 99)));
    CHECK_FALSE(undoStack.isClean());

    CHECK(undoStack.undo());
    CHECK(undoStack.isClean());

    CHECK(undoStack.redo());
    CHECK_FALSE(undoStack.isClean());

    undoStack.setCleanPoint();
    CHECK(undoStack.isClean());
}

// The doc 12 (M1 acceptance) property test: a random sequence of commands
// through the UndoStack, undone all the way back, must restore the model
// to exactly the state it was in before the sequence started.
TEST_CASE("UndoStack property: random commands, undo all, model restored exactly (10k iterations)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    UndoStack undoStack(model);
    // The default limit (500) is a real product feature (bounded undo
    // history) that would otherwise evict early entries long before all
    // 10k get a chance to be undone, breaking the "undo everything"
    // premise of this test -- raise it for this run only.
    undoStack.limit = 20'000;

    Model snapshot = model; // the state the whole sequence must revert back to

    std::mt19937 rng(2026);
    std::vector<ClipId> liveClips;
    FrameIndex nextFreePosition = 0;
    int appliedCount = 0;

    for (int i = 0; i < 10'000; ++i) {
        std::uniform_int_distribution<int> pickAction(0, liveClips.empty() ? 0 : 3);
        int action = pickAction(rng);
        bool executed = false;

        if (action == 0 || liveClips.empty()) {
            FrameIndex length = 10 + static_cast<FrameIndex>(rng() % 90);
            auto cmd = std::make_unique<InsertClip>(track, asset, nextFreePosition, 0, length - 1);
            InsertClip *raw = cmd.get();
            executed = undoStack.execute(std::move(cmd));
            if (executed) {
                nextFreePosition += length;
                liveClips.push_back(raw->clipId());
            }
        } else if (action == 1) {
            std::uniform_int_distribution<size_t> pickClip(0, liveClips.size() - 1);
            size_t index = pickClip(rng);
            executed = undoStack.execute(std::make_unique<RemoveClip>(liveClips[index]));
            if (executed)
                liveClips.erase(liveClips.begin() + static_cast<std::ptrdiff_t>(index));
        } else if (action == 2) {
            std::uniform_int_distribution<size_t> pickClip(0, liveClips.size() - 1);
            ClipId clip = liveClips[pickClip(rng)];
            FrameIndex length = model.clip(clip).length();
            executed = undoStack.execute(std::make_unique<MoveClip>(clip, track, nextFreePosition));
            if (executed)
                nextFreePosition += length;
        } else {
            std::uniform_int_distribution<size_t> pickClip(0, liveClips.size() - 1);
            ClipId clip = liveClips[pickClip(rng)];
            const Clip &current = model.clip(clip);
            if (current.length() > 2) {
                FrameIndex at = current.position + 1 + static_cast<FrameIndex>(rng() % (current.length() - 2));
                auto cmd = std::make_unique<SplitClip>(clip, at);
                SplitClip *raw = cmd.get();
                executed = undoStack.execute(std::move(cmd));
                if (executed)
                    liveClips.push_back(raw->rightId());
            }
        }

        if (executed)
            ++appliedCount;
    }

    REQUIRE(appliedCount > 0);
    for (int i = 0; i < appliedCount; ++i)
        REQUIRE(undoStack.undo());

    CHECK_FALSE(undoStack.canUndo());
    CHECK(equalIgnoringIdAllocator(model, snapshot));
}
