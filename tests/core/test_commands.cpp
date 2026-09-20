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

TEST_CASE("SplitAudio: reuses an existing empty audio track and fully reverts")
{
    Model model = Model::createEmpty();
    TrackId video = model.addTrack(Track::Kind::Video, 0, "V1");
    TrackId audio = model.addTrack(Track::Kind::Audio, 1, "A1");
    AssetId asset = addTestAsset(model);
    ClipId clip = model.insertClip(video, asset, 10, 0, 99);
    Model before = model;

    SplitAudio cmd(clip);
    REQUIRE(cmd.apply(model));

    CHECK(model.clip(clip).videoEnabled);
    CHECK_FALSE(model.clip(clip).audioEnabled);
    REQUIRE(model.hasClip(cmd.audioClipId()));
    const Clip &audioClip = model.clip(cmd.audioClipId());
    CHECK(audioClip.track == audio);
    CHECK_FALSE(audioClip.videoEnabled);
    CHECK(audioClip.audioEnabled);
    CHECK(audioClip.position == 10);
    CHECK(audioClip.in == 0);
    CHECK(audioClip.out == 99);
    // No new track: the existing empty one was reused.
    CHECK(model.sequence().tracks.size() == 2);

    cmd.revert(model);
    CHECK_FALSE(model.hasClip(cmd.audioClipId()));
    CHECK(equalIgnoringIdAllocator(model, before));
}

TEST_CASE("SplitAudio: creates a new audio track when none exists; revert leaves it in place")
{
    Model model = Model::createEmpty();
    TrackId video = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId clip = model.insertClip(video, asset, 0, 0, 99);
    size_t tracksBefore = model.sequence().tracks.size();

    SplitAudio cmd(clip);
    REQUIRE(cmd.apply(model));
    REQUIRE(model.sequence().tracks.size() == tracksBefore + 1);
    TrackId newAudioTrack = model.clip(cmd.audioClipId()).track;
    CHECK(model.track(newAudioTrack).kind == Track::Kind::Audio);

    // Doc 04's revert entry for SplitAudio is "RemoveClip(audioClipId) +
    // restore audio on the original clip" -- it does not remove a track
    // apply() created, so the track is deliberately left behind, empty.
    cmd.revert(model);
    CHECK(model.hasTrack(newAudioTrack));
    CHECK(model.track(newAudioTrack).clips.empty());
    CHECK(model.clip(clip).audioEnabled);
    CHECK(model.clip(clip).videoEnabled);
}

TEST_CASE("SplitAudio: skips an occupied audio track for a later free one")
{
    Model model = Model::createEmpty();
    TrackId video = model.addTrack(Track::Kind::Video, 0, "V1");
    TrackId occupiedAudio = model.addTrack(Track::Kind::Audio, 1, "A1");
    TrackId freeAudio = model.addTrack(Track::Kind::Audio, 2, "A2");
    AssetId asset = addTestAsset(model);
    ClipId clip = model.insertClip(video, asset, 0, 0, 99);
    // Occupy the first audio track over the exact span the split needs.
    model.insertClip(occupiedAudio, asset, 0, 0, 99);

    SplitAudio cmd(clip);
    REQUIRE(cmd.apply(model));
    CHECK(model.clip(cmd.audioClipId()).track == freeAudio);
    CHECK(model.sequence().tracks.size() == 3); // no track created -- freeAudio had room
}

TEST_CASE("SplitAudio: refuses a clip with no audio to split")
{
    Model model = Model::createEmpty();
    TrackId video = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId clip = model.insertClip(video, asset, 0, 0, 99);
    model.setClipEnabled(clip, /*videoEnabled=*/true, /*audioEnabled=*/false);

    SplitAudio cmd(clip);
    CHECK_FALSE(cmd.apply(model));
}

TEST_CASE("SplitAudio: refuses a clip that is already audio-only")
{
    Model model = Model::createEmpty();
    TrackId audio = model.addTrack(Track::Kind::Audio, 0, "A1");
    AssetId asset = addTestAsset(model);
    ClipId clip = model.insertClip(audio, asset, 0, 0, 99);
    model.setClipEnabled(clip, /*videoEnabled=*/false, /*audioEnabled=*/true);

    SplitAudio cmd(clip);
    CHECK_FALSE(cmd.apply(model));
}

TEST_CASE("AddTrack / RemoveTrack: revert restores the track's clips too")
{
    Model model = Model::createEmpty();
    Model before = model;

    AddTrack addCmd(Track::Kind::Video, 0, "V1");
    REQUIRE(addCmd.apply(model));
    // RemoveTrack refuses to leave the sequence with zero tracks (matching
    // v1's MltEngine::removeTrack), so a second track keeps this test
    // exercising apply()/revert()'s mechanics rather than that refusal.
    AddTrack secondTrackCmd(Track::Kind::Video, 1, "V2");
    REQUIRE(secondTrackCmd.apply(model));
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

TEST_CASE("RemoveTrack: refuses to remove the sequence's last remaining track")
{
    Model model = Model::createEmpty();
    AddTrack addCmd(Track::Kind::Video, 0, "V1");
    REQUIRE(addCmd.apply(model));

    RemoveTrack removeCmd(addCmd.trackId());
    CHECK_FALSE(removeCmd.apply(model));
    CHECK(model.hasTrack(addCmd.trackId()));
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

TEST_CASE("SetTrackVolume: apply then revert restores an equal model; consecutive same-track sets merge")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Audio, 0, "A1");
    Model before = model;

    SetTrackVolume cmd(track, 0.5);
    REQUIRE(cmd.apply(model));
    CHECK(model.track(track).volume == 0.5);

    SetTrackVolume second(track, 0.25);
    REQUIRE(second.apply(model)); // simulates UndoStack::execute() applying it before offering the merge
    REQUIRE(cmd.mergeWith(second));
    CHECK(model.track(track).volume == 0.25); // second's own apply() already landed this

    cmd.revert(model);
    CHECK(model == before); // reverting the merged command undoes back to the pre-drag start, not just the last tick
}

TEST_CASE("SetTrackVolume: does not merge across different tracks")
{
    Model model = Model::createEmpty();
    TrackId trackA = model.addTrack(Track::Kind::Audio, 0, "A1");
    TrackId trackB = model.addTrack(Track::Kind::Audio, 1, "A2");

    SetTrackVolume cmd(trackA, 0.5);
    REQUIRE(cmd.apply(model));
    SetTrackVolume other(trackB, 0.5);
    REQUIRE(other.apply(model));
    CHECK_FALSE(cmd.mergeWith(other));
}

TEST_CASE("Locked tracks refuse insert/move/resize/split/remove, but not the toggle itself")
{
    Model model = Model::createEmpty();
    TrackId locked = model.addTrack(Track::Kind::Video, 0, "V1");
    TrackId other = model.addTrack(Track::Kind::Video, 1, "V2");
    AssetId asset = addTestAsset(model);
    ClipId clip = model.insertClip(locked, asset, 0, 0, 99);

    REQUIRE(SetTrackFlags(locked, false, false, true).apply(model)); // lock it
    CHECK(model.track(locked).locked);

    CHECK_FALSE(InsertClip(locked, asset, 200, 0, 99).apply(model));
    CHECK_FALSE(RemoveClip(clip).apply(model));
    CHECK_FALSE(ResizeClip(clip, 0, 49, 0).apply(model));
    CHECK_FALSE(SplitClip(clip, 50).apply(model));
    CHECK_FALSE(SplitAudio(clip).apply(model));
    // Both directions: moving a clip onto a locked track, and moving the
    // locked track's own clip elsewhere, must each refuse.
    ClipId otherClip = model.insertClip(other, asset, 300, 0, 99);
    CHECK_FALSE(MoveClip(otherClip, locked, 400).apply(model));
    CHECK_FALSE(MoveClip(clip, other, 400).apply(model));

    // Unlocking is never blocked by the lock it's about to clear.
    CHECK(SetTrackFlags(locked, false, false, false).apply(model));
    CHECK_FALSE(model.track(locked).locked);
    CHECK(MoveClip(clip, locked, 500).apply(model)); // now allowed
}

TEST_CASE("SplitAudio skips a locked audio track and creates a new one instead")
{
    Model model = Model::createEmpty();
    TrackId video = model.addTrack(Track::Kind::Video, 0, "V1");
    TrackId lockedAudio = model.addTrack(Track::Kind::Audio, 1, "A1");
    REQUIRE(SetTrackFlags(lockedAudio, false, false, true).apply(model));
    AssetId asset = addTestAsset(model);
    ClipId clip = model.insertClip(video, asset, 0, 0, 99);

    SplitAudio cmd(clip);
    REQUIRE(cmd.apply(model));
    TrackId destination = model.clip(cmd.audioClipId()).track;
    CHECK(destination != lockedAudio);
    CHECK(model.sequence().tracks.size() == 3); // a new track was created, the locked one was skipped
}

TEST_CASE("RemoveTrack refuses a locked track")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    model.addTrack(Track::Kind::Video, 1, "V2"); // so "last track" isn't the reason for refusal
    REQUIRE(SetTrackFlags(track, false, false, true).apply(model));

    CHECK_FALSE(RemoveTrack(track).apply(model));
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

    auto first = std::make_unique<InsertClip>(track, asset, 0, 0, 99);
    InsertClip *firstCmd = first.get();
    auto second = std::make_unique<InsertClip>(track, asset, 200, 0, 99);
    InsertClip *secondCmd = second.get();

    Transaction tx(model);
    REQUIRE(tx.run(std::move(first)));
    REQUIRE(tx.run(std::move(second)));
    tx.commit();

    // Not just a count: a commit bug that landed both clips at the same
    // position, or dropped one and duplicated the other, would still leave
    // clips.size() == 2. Track::clips is kept sorted by position
    // (Model::sortTrackClips), so checking both ids and positions directly
    // confirms each command's own effect actually landed, not just that
    // two clips of *some* shape exist.
    REQUIRE(model.track(track).clips.size() == 2);
    CHECK(model.track(track).clips[0] == firstCmd->clipId());
    CHECK(model.track(track).clips[1] == secondCmd->clipId());
    CHECK(model.clip(firstCmd->clipId()).position == 0);
    CHECK(model.clip(secondCmd->clipId()).position == 200);
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
