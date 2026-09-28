#include "doctest.h"

#include "core/commands/primitives.h"
#include "core/commands/transaction.h"
#include "core/commands/undo_stack.h"
#include "core/model/model.h"
#include "random_commands.h"

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

TEST_CASE("InsertClip onto an audio track disables video but leaves audio on (audit A4)")
{
    Model model = Model::createEmpty();
    TrackId audioTrack = model.addTrack(Track::Kind::Audio, 0, "A1");
    AssetId asset = addTestAsset(model); // hasVideo=hasAudio=true

    InsertClip cmd(audioTrack, asset, 0, 0, 99);
    REQUIRE(cmd.apply(model));
    const Clip &inserted = model.clip(cmd.clipId());
    CHECK_FALSE(inserted.videoEnabled);
    CHECK(inserted.audioEnabled);
    CHECK(model.check().empty()); // invariant 8 would otherwise flag this
}

TEST_CASE("MoveClip onto an audio track disables video and reverting restores it (audit A4)")
{
    Model model = Model::createEmpty();
    TrackId videoTrack = model.addTrack(Track::Kind::Video, 0, "V1");
    TrackId audioTrack = model.addTrack(Track::Kind::Audio, 1, "A1");
    AssetId asset = addTestAsset(model); // hasVideo=hasAudio=true
    ClipId clip = model.insertClip(videoTrack, asset, 0, 0, 99);
    REQUIRE(model.clip(clip).videoEnabled);
    Model before = model;

    MoveClip cmd(clip, audioTrack, 0);
    REQUIRE(cmd.apply(model));
    CHECK_FALSE(model.clip(clip).videoEnabled);
    CHECK(model.clip(clip).audioEnabled);
    CHECK(model.check().empty());

    cmd.revert(model);
    CHECK(model.clip(clip).videoEnabled); // not just moved back -- video re-enabled too
    CHECK(model == before);
}

TEST_CASE("MoveClip refuses landing a video-only clip on an audio track (audit A4)")
{
    Model model = Model::createEmpty();
    TrackId videoTrack = model.addTrack(Track::Kind::Video, 0, "V1");
    TrackId audioTrack = model.addTrack(Track::Kind::Audio, 1, "A1");
    AssetId asset = addTestAsset(model);
    ClipId clip = model.insertClip(videoTrack, asset, 0, 0, 99);
    // Make the clip itself video-only even though its asset has audio, the
    // same end state SplitAudio's video-only half is left in -- moving it
    // onto an audio track would contribute neither picture nor sound.
    model.setClipEnabled(clip, /*videoEnabled=*/true, /*audioEnabled=*/false);
    Model before = model;

    MoveClip cmd(clip, audioTrack, 0);
    CHECK_FALSE(cmd.apply(model));
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

TEST_CASE("InsertClip: cutting past a boundless asset's recorded length extends it (audit E3)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset stillAsset;
    stillAsset.displayName = "watermark.png";
    stillAsset.info.isStillImage = true;
    stillAsset.info.lengthInSequenceFrames = 100;
    AssetId assetId = model.addAsset(stillAsset);

    InsertClip cmd(track, assetId, 0, 0, 499); // 500 frames, past the recorded 100
    REQUIRE(cmd.apply(model));
    CHECK(model.asset(assetId).info.lengthInSequenceFrames == 500);
}

TEST_CASE("InsertClip: a shorter cut never shrinks a boundless asset's recorded length")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset stillAsset;
    stillAsset.displayName = "watermark.png";
    stillAsset.info.isStillImage = true;
    stillAsset.info.lengthInSequenceFrames = 1000;
    AssetId assetId = model.addAsset(stillAsset);

    InsertClip cmd(track, assetId, 0, 0, 49); // 50 frames, well under the recorded 1000
    REQUIRE(cmd.apply(model));
    CHECK(model.asset(assetId).info.lengthInSequenceFrames == 1000);
}

TEST_CASE("ResizeClip: extending a boundless clip past the asset's recorded length extends it (audit E3)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset stillAsset;
    stillAsset.displayName = "watermark.png";
    stillAsset.info.isStillImage = true;
    stillAsset.info.lengthInSequenceFrames = 100;
    AssetId assetId = model.addAsset(stillAsset);
    ClipId clip = model.insertClip(track, assetId, 0, 0, 99);

    ResizeClip cmd(clip, 0, 599, 0); // 600 frames, past the recorded 100
    REQUIRE(cmd.apply(model));
    CHECK(model.asset(assetId).info.lengthInSequenceFrames == 600);
}

TEST_CASE("InsertClip: revert restores the asset's recorded length, not just the clip (2026-09-22 audit C1)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset stillAsset;
    stillAsset.displayName = "watermark.png";
    stillAsset.info.isStillImage = true;
    stillAsset.info.lengthInSequenceFrames = 100;
    AssetId assetId = model.addAsset(stillAsset);
    Model before = model;

    InsertClip cmd(track, assetId, 0, 0, 499); // 500 frames, past the recorded 100
    REQUIRE(cmd.apply(model));
    REQUIRE(model.asset(assetId).info.lengthInSequenceFrames == 500);

    cmd.revert(model);
    CHECK(model.asset(assetId).info.lengthInSequenceFrames == 100); // back to what it was, not left at 500
    CHECK(equalIgnoringIdAllocator(model, before));
}

TEST_CASE("InsertClip: revert doesn't shrink the asset if another clip already extended it further "
         "(2026-09-22 audit C1)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset stillAsset;
    stillAsset.displayName = "watermark.png";
    stillAsset.info.isStillImage = true;
    stillAsset.info.lengthInSequenceFrames = 100;
    AssetId assetId = model.addAsset(stillAsset);

    // A second, independent clip cuts even further into the same asset
    // AFTER the first -- its own recorded extension (800) must survive
    // the first clip's revert.
    InsertClip firstCmd(track, assetId, 0, 0, 499); // extends to 500
    REQUIRE(firstCmd.apply(model));
    InsertClip secondCmd(track, assetId, 500, 0, 799); // extends to 800
    REQUIRE(secondCmd.apply(model));
    REQUIRE(model.asset(assetId).info.lengthInSequenceFrames == 800);

    firstCmd.revert(model);
    // Must NOT drop to 100 (firstCmd's own pre-apply value) -- the
    // second clip's own, still-live extension to 800 is the true
    // current requirement.
    CHECK(model.asset(assetId).info.lengthInSequenceFrames == 800);
}

TEST_CASE("ResizeClip: revert restores the asset's recorded length too (2026-09-22 audit C1)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset stillAsset;
    stillAsset.displayName = "watermark.png";
    stillAsset.info.isStillImage = true;
    stillAsset.info.lengthInSequenceFrames = 100;
    AssetId assetId = model.addAsset(stillAsset);
    ClipId clip = model.insertClip(track, assetId, 0, 0, 99);
    Model before = model;

    ResizeClip cmd(clip, 0, 599, 0); // 600 frames, past the recorded 100
    REQUIRE(cmd.apply(model));
    REQUIRE(model.asset(assetId).info.lengthInSequenceFrames == 600);

    cmd.revert(model);
    CHECK(model.asset(assetId).info.lengthInSequenceFrames == 100);
    CHECK(equalIgnoringIdAllocator(model, before));
}

TEST_CASE("InsertClip refuses a negative in point even for a boundless asset (audit C1)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset stillAsset;
    stillAsset.displayName = "watermark.png";
    stillAsset.info.isStillImage = true;
    stillAsset.info.lengthInSequenceFrames = 100;
    AssetId assetId = model.addAsset(stillAsset);
    Model before = model;

    InsertClip cmd(track, assetId, 0, -10, 99);
    CHECK_FALSE(cmd.apply(model));
    CHECK(model == before);
}

TEST_CASE("ResizeClip refuses a negative in point even for a boundless asset (audit C1)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset stillAsset;
    stillAsset.displayName = "watermark.png";
    stillAsset.info.isStillImage = true;
    stillAsset.info.lengthInSequenceFrames = 100;
    AssetId assetId = model.addAsset(stillAsset);
    ClipId clip = model.insertClip(track, assetId, 0, 0, 99);
    Model before = model;

    ResizeClip cmd(clip, -5, 99, 0);
    CHECK_FALSE(cmd.apply(model));
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

TEST_CASE("SplitClip: revert restores the left clip's fadeOut, not just its out point (audit C5)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId clip = model.insertClip(track, asset, 0, 0, 99);
    model.setClipFadeOut(clip, FadeSpec{10});
    Model before = model;

    SplitClip cmd(clip, 40);
    REQUIRE(cmd.apply(model));
    // Model::splitClip() clears the left half's fadeOut: the split point is
    // now a hard cut, not a fade.
    CHECK_FALSE(model.clip(clip).fadeOut.has_value());

    cmd.revert(model);
    REQUIRE(model.clip(clip).fadeOut.has_value());
    CHECK(model.clip(clip).fadeOut->length == 10);
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

TEST_CASE("AddAsset / RemoveAsset: revert restores the asset and every clip that used it")
{
    Model model = Model::createEmpty();
    AddTrack addTrackCmd(Track::Kind::Video, 0, "V1");
    REQUIRE(addTrackCmd.apply(model));

    Asset asset;
    asset.displayName = "clip.mp4";
    asset.info.lengthInSequenceFrames = 1000;
    AddAsset addAssetCmd(asset);
    REQUIRE(addAssetCmd.apply(model));

    ClipId clipA = model.insertClip(addTrackCmd.trackId(), addAssetCmd.assetId(), 0, 0, 99);
    ClipId clipB = model.insertClip(addTrackCmd.trackId(), addAssetCmd.assetId(), 100, 0, 49);

    Model beforeRemoval = model;
    RemoveAsset removeCmd(addAssetCmd.assetId());
    REQUIRE(removeCmd.apply(model));
    CHECK_FALSE(model.hasAsset(addAssetCmd.assetId()));
    CHECK_FALSE(model.hasClip(clipA));
    CHECK_FALSE(model.hasClip(clipB));

    removeCmd.revert(model);
    // Not model == beforeRemoval: addAsset()'s reuseId.value_or(AssetId{
    // allocateId()}) evaluates allocateId() unconditionally (a value_or
    // argument is a regular function argument, not short-circuited), so
    // every reuse-by-id restore still burns and discards one id, same as
    // any other id-allocating command's own apply/revert cycle --
    // equalIgnoringIdAllocator's own comment above.
    CHECK(equalIgnoringIdAllocator(model, beforeRemoval));
}

TEST_CASE("RemoveAsset strips a dissolve on one of its clips instead of leaving it dangling (2026-09-23 audit C1)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId a = model.insertClip(track, asset, 0, 0, 49);
    ClipId b = model.insertClip(track, asset, 50, 100, 149);
    AddTransition addCmd(track, a, b, 6, 4);
    REQUIRE(addCmd.apply(model));

    // Removing the asset removes both a and b (both cut from it) --
    // before this fix, RemoveAsset called Model::removeClip directly,
    // never stripping the transition first, so it vanished along with
    // whichever clip happened to be removed while the OTHER clip (if it
    // had survived) would have kept its extension. Here both clips are
    // gone either way, but check() must still report a clean model, not
    // a transition referencing a missing clip (invariant 6).
    RemoveAsset removeCmd(asset);
    REQUIRE(removeCmd.apply(model));
    CHECK_FALSE(model.hasAsset(asset));
    CHECK_FALSE(model.hasClip(a));
    CHECK_FALSE(model.hasClip(b));
    CHECK_FALSE(model.hasTransition(addCmd.transitionId()));
    CHECK(model.check().empty());

    removeCmd.revert(model);
    REQUIRE(model.hasTransition(addCmd.transitionId()));
    CHECK(model.clip(a).out == 55); // the dissolve's extension is back too, not just the clips
    CHECK(model.check().empty());
}

TEST_CASE("RemoveAsset strips a dissolve to another clip that SURVIVES the removal (2026-09-23 audit C1)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId assetA = addTestAsset(model);
    Asset otherAsset;
    otherAsset.displayName = "other.mp4";
    otherAsset.info.lengthInSequenceFrames = 100'000;
    AssetId assetB = model.addAsset(otherAsset);

    ClipId a = model.insertClip(track, assetA, 0, 0, 49);
    ClipId b = model.insertClip(track, assetB, 50, 100, 149);
    AddTransition addCmd(track, a, b, 6, 4);
    REQUIRE(addCmd.apply(model));

    // Only assetA (and clip a) is removed; b survives on a different
    // asset. Without stripping first, b would have kept its position
    // pulled back by extendB while the transition record vanished with
    // a -- exactly the "other clip keeps its dissolve extension" bug
    // the audit reported.
    RemoveAsset removeCmd(assetA);
    REQUIRE(removeCmd.apply(model));
    CHECK_FALSE(model.hasClip(a));
    REQUIRE(model.hasClip(b));
    CHECK(model.clip(b).position == 50); // back to its own un-extended position
    CHECK_FALSE(model.hasTransition(addCmd.transitionId()));
    CHECK(model.check().empty());

    removeCmd.revert(model);
    REQUIRE(model.hasTransition(addCmd.transitionId()));
    CHECK(model.clip(b).position == 46);
    CHECK(model.check().empty());
}

TEST_CASE("RemoveAsset refuses the whole removal when a referencing clip is on a locked track")
{
    Model model = Model::createEmpty();
    AddTrack addTrackCmd(Track::Kind::Video, 0, "V1");
    REQUIRE(addTrackCmd.apply(model));
    AssetId asset = addTestAsset(model);
    ClipId clip = model.insertClip(addTrackCmd.trackId(), asset, 0, 0, 99);

    model.setTrackFlags(addTrackCmd.trackId(), /*muted=*/false, /*hidden=*/false, /*locked=*/true);

    RemoveAsset removeCmd(asset);
    CHECK_FALSE(removeCmd.apply(model));
    CHECK(model.hasAsset(asset));
    CHECK(model.hasClip(clip));
}

TEST_CASE("RemoveAsset with no referencing clips just removes the asset")
{
    Model model = Model::createEmpty();
    AssetId asset = addTestAsset(model);

    RemoveAsset removeCmd(asset);
    REQUIRE(removeCmd.apply(model));
    CHECK_FALSE(model.hasAsset(asset));

    removeCmd.revert(model);
    CHECK(model.hasAsset(asset));
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

TEST_CASE("RenameTrack: apply then revert restores an equal model")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Model before = model;

    RenameTrack cmd(track, "Interview");
    REQUIRE(cmd.apply(model));
    CHECK(model.track(track).name == "Interview");

    cmd.revert(model);
    CHECK(model == before);
}

TEST_CASE("RenameTrack: not blocked by a locked track")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    REQUIRE(SetTrackFlags(track, false, false, true).apply(model)); // lock it

    RenameTrack cmd(track, "B-roll");
    CHECK(cmd.apply(model));
    CHECK(model.track(track).name == "B-roll");
}

TEST_CASE("RenameClip: apply then revert restores an equal model; a shared asset's other clip is untouched")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId clipA = model.insertClip(track, asset, 0, 0, 99);
    ClipId clipB = model.insertClip(track, asset, 200, 0, 99);
    std::string originalName = model.clip(clipA).name; // both start as the asset's displayName
    Model before = model;

    RenameClip cmd(clipA, "Take 2");
    REQUIRE(cmd.apply(model));
    CHECK(model.clip(clipA).name == "Take 2");
    // Different clips from the same source can have different names --
    // renaming one never touches another clip of the same asset.
    CHECK(model.clip(clipB).name == originalName);

    cmd.revert(model);
    CHECK(model == before);
}

TEST_CASE("RenameClip: not blocked by a locked track")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId clip = model.insertClip(track, asset, 0, 0, 99);
    REQUIRE(SetTrackFlags(track, false, false, true).apply(model)); // lock it

    RenameClip cmd(clip, "Take 2");
    CHECK(cmd.apply(model));
    CHECK(model.clip(clip).name == "Take 2");
}

TEST_CASE("AddTransition: apply then revert restores an equal model")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId a = model.insertClip(track, asset, 0, 0, 49);     // [0, 50), source [0, 49]
    ClipId b = model.insertClip(track, asset, 50, 100, 149); // [50, 100), source [100, 149]
    Model before = model;

    AddTransition cmd(track, a, b, 6, 4);
    REQUIRE(cmd.apply(model));
    CHECK(model.hasTransition(cmd.transitionId()));
    CHECK(model.transition(cmd.transitionId()).length == 10);
    CHECK(model.check().empty());

    cmd.revert(model);
    CHECK(equalIgnoringIdAllocator(model, before));
}

TEST_CASE("AddTransition refuses a zero-length request")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId a = model.insertClip(track, asset, 0, 0, 49);
    ClipId b = model.insertClip(track, asset, 50, 100, 149);

    CHECK_FALSE(AddTransition(track, a, b, 0, 0).apply(model));
}

TEST_CASE("AddTransition refuses clips that aren't exactly adjacent")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId a = model.insertClip(track, asset, 0, 0, 49);
    ClipId b = model.insertClip(track, asset, 60, 100, 149); // a 10-frame gap, not touching
    Model before = model;

    CHECK_FALSE(AddTransition(track, a, b, 6, 4).apply(model));
    CHECK(model == before);
}

TEST_CASE("AddTransition refuses when clip a lacks tail handle for the requested extension")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model, 56); // source is exactly [0, 55]
    ClipId a = model.insertClip(track, asset, 0, 0, 49);    // 6 frames of tail handle left (50..55)
    ClipId b = model.insertClip(track, asset, 50, 0, 49);   // a second clip from the same asset
    Model before = model;

    CHECK_FALSE(AddTransition(track, a, b, 7, 0).apply(model)); // only 6 frames available
    CHECK(model == before);
    CHECK(AddTransition(track, a, b, 6, 0).apply(model)); // exactly the available handle
}

TEST_CASE("AddTransition refuses when clip b lacks head handle for the requested extension")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId a = model.insertClip(track, asset, 0, 0, 49);
    ClipId b = model.insertClip(track, asset, 50, 0, 49); // in == 0, no head handle at all
    Model before = model;

    CHECK_FALSE(AddTransition(track, a, b, 0, 1).apply(model));
    CHECK(model == before);
}

TEST_CASE("RemoveTransition: apply then revert restores an equal model, regardless of the original split")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId a = model.insertClip(track, asset, 0, 0, 49);
    ClipId b = model.insertClip(track, asset, 50, 100, 149);
    TransitionId t = model.addTransition(track, a, b, 6, 4); // built directly, not via the command under test
    Model before = model;

    RemoveTransition cmd(t);
    REQUIRE(cmd.apply(model));
    CHECK_FALSE(model.hasTransition(t));
    CHECK(model.check().empty());

    cmd.revert(model);
    // Not plain ==: reuseId.value_or(TransitionId{allocateId()}) (like
    // every other reuseId-taking mutator in this codebase) evaluates its
    // fallback eagerly, so revert() burns one id slot even though it ends
    // up reusing `t` -- the same reason InsertClip/SplitClip's own
    // apply-then-revert tests use this helper instead of ==.
    CHECK(equalIgnoringIdAllocator(model, before));
}

TEST_CASE("RemoveTransition refuses on a locked track")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId a = model.insertClip(track, asset, 0, 0, 49);
    ClipId b = model.insertClip(track, asset, 50, 100, 149);
    AddTransition addCmd(track, a, b, 6, 4);
    REQUIRE(addCmd.apply(model));

    REQUIRE(SetTrackFlags(track, false, false, true).apply(model)); // lock it after creating the transition

    CHECK_FALSE(RemoveTransition(addCmd.transitionId()).apply(model));
}

// --- T1 (2026-09-22 audit): commands that touch a transition-linked clip ---

TEST_CASE("RemoveClip strips an existing transition first and restores it on revert (audit T1)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId a = model.insertClip(track, asset, 0, 0, 49);     // [0, 50), source [0, 49]
    ClipId b = model.insertClip(track, asset, 50, 100, 149); // [50, 100), source [100, 149]
    AddTransition addCmd(track, a, b, 6, 4);
    REQUIRE(addCmd.apply(model));
    Model linked = model;

    RemoveClip cmd(b);
    REQUIRE(cmd.apply(model));
    CHECK_FALSE(model.hasClip(b));
    CHECK_FALSE(model.hasTransition(addCmd.transitionId()));
    CHECK(model.clip(a).out == 49); // a's tail handle shrank back too
    CHECK(model.check().empty());

    cmd.revert(model);
    CHECK(equalIgnoringIdAllocator(model, linked));
}

TEST_CASE("MoveClip strips an existing transition first and restores it on revert (audit T1)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId a = model.insertClip(track, asset, 0, 0, 49);
    ClipId b = model.insertClip(track, asset, 50, 100, 149);
    AddTransition addCmd(track, a, b, 6, 4);
    REQUIRE(addCmd.apply(model));
    Model linked = model;

    MoveClip cmd(b, track, 200);
    REQUIRE(cmd.apply(model));
    CHECK_FALSE(model.hasTransition(addCmd.transitionId()));
    CHECK(model.clip(a).out == 49);
    CHECK(model.clip(b).position == 200);
    CHECK(model.check().empty());

    cmd.revert(model);
    CHECK(equalIgnoringIdAllocator(model, linked));
}

TEST_CASE("MoveClip refuses a no-op move to its own track and position (2026-09-23 audit C3)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId a = model.insertClip(track, asset, 0, 0, 49);
    ClipId b = model.insertClip(track, asset, 50, 100, 149);
    AddTransition addCmd(track, a, b, 6, 4);
    REQUIRE(addCmd.apply(model));
    Model linked = model;

    // A drag that ends exactly where it started (a small wobble past the
    // pixel-level "trivial drag" threshold in the app layer, but landing
    // back on the same track/position) must not strip b's dissolve for
    // an edit that changes nothing.
    MoveClip cmd(b, track, model.clip(b).position);
    CHECK_FALSE(cmd.apply(model));
    CHECK(equalIgnoringIdAllocator(model, linked));
    CHECK(model.hasTransition(addCmd.transitionId()));
}

TEST_CASE("ResizeClip strips an existing transition first and restores it on revert (audit T1)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId a = model.insertClip(track, asset, 0, 0, 49);
    ClipId b = model.insertClip(track, asset, 50, 100, 149);
    AddTransition addCmd(track, a, b, 6, 4);
    REQUIRE(addCmd.apply(model));
    Model linked = model;

    // Shrink b to 20 frames, moved clear of a's still-extended tail (a
    // ends at 55 at this point -- the strip that shrinks it back to 49
    // hasn't happened yet when this range is validated).
    ResizeClip cmd(b, 100, 119, 60);
    REQUIRE(cmd.apply(model));
    CHECK_FALSE(model.hasTransition(addCmd.transitionId()));
    CHECK(model.clip(a).out == 49);
    CHECK(model.clip(b).in == 100);
    CHECK(model.clip(b).out == 119);
    CHECK(model.clip(b).position == 60);
    CHECK(model.check().empty());

    cmd.revert(model);
    CHECK(equalIgnoringIdAllocator(model, linked));
}

TEST_CASE("ResizeClip allows a tail trim far from an untouched incoming dissolve (2026-09-23 audit C2)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId a = model.insertClip(track, asset, 0, 0, 49);
    ClipId b = model.insertClip(track, asset, 50, 100, 149);
    AddTransition addCmd(track, a, b, 6, 4); // b's incoming overlap: [46, 56); b's current span [46,100)
    REQUIRE(addCmd.apply(model));

    // Trim only b's tail (in/position unchanged, well away from the
    // incoming dissolve at its head) -- previously always refused, since
    // isRangeFree checked the new range against a's still-extended
    // [0,55) span and found the legitimate [46,55) dissolve overlap
    // "occupied" (audit C2's exact repro: "trim B tail by 5, far from
    // the dissolve: ok=0").
    ResizeClip cmd(b, 96, 139, 46);
    REQUIRE(cmd.apply(model));
    CHECK(model.hasTransition(addCmd.transitionId())); // untouched
    CHECK(model.clip(a).out == 55);                    // a's extension survives completely undisturbed
    CHECK(model.clip(b).out == 139);
    CHECK(model.check().empty());

    cmd.revert(model);
    CHECK(model.hasTransition(addCmd.transitionId()));
    CHECK(model.clip(b).out == 149);
}

TEST_CASE("ResizeClip allows a head trim far from an untouched outgoing dissolve (2026-09-23 audit C2)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId a = model.insertClip(track, asset, 0, 0, 99);     // [0,100)
    ClipId b = model.insertClip(track, asset, 100, 50, 149); // head handle to spare
    AddTransition addCmd(track, a, b, 6, 4); // a's outgoing overlap: [96, 106); a's current span [0,106)
    REQUIRE(addCmd.apply(model));

    // Trim only a's head (out unchanged, well away from the outgoing
    // dissolve at its tail).
    ResizeClip cmd(a, 10, 105, 10);
    REQUIRE(cmd.apply(model));
    CHECK(model.hasTransition(addCmd.transitionId())); // untouched
    CHECK(model.clip(b).position == 96);               // b's extension survives completely undisturbed
    CHECK(model.clip(a).in == 10);
    CHECK(model.check().empty());

    cmd.revert(model);
    CHECK(model.hasTransition(addCmd.transitionId()));
    CHECK(model.clip(a).in == 0);
}

TEST_CASE("SplitClip preserves an incoming transition on the left half when the split is far from it (audit C4)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId a = model.insertClip(track, asset, 0, 0, 49);
    ClipId b = model.insertClip(track, asset, 50, 100, 149);
    AddTransition addCmd(track, a, b, 6, 4); // b's incoming overlap: [46, 56)
    REQUIRE(addCmd.apply(model));

    // 70 is well past the overlap's own end (56) -- the split must not
    // touch this dissolve at all (audit C4: T1's original fix stripped
    // every transition on the clip regardless of where the split point
    // fell, destroying dissolves nowhere near the cut).
    SplitClip cmd(b, 70);
    REQUIRE(cmd.apply(model));
    CHECK(model.hasTransition(addCmd.transitionId()));
    CHECK(model.transition(addCmd.transitionId()).b == b); // still the left half, same id, untouched
    CHECK(model.clip(a).out == 55);                        // a's extension survives completely undisturbed
    CHECK(model.clip(b).position == 46);                   // left half keeps the original head geometry
    CHECK(model.check().empty());

    cmd.revert(model);
    CHECK(model.hasTransition(addCmd.transitionId()));
    CHECK_FALSE(model.hasClip(cmd.rightId()));
}

TEST_CASE("SplitClip strips an incoming transition when the split point falls inside its own overlap (audit C4)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId a = model.insertClip(track, asset, 0, 0, 49);
    ClipId b = model.insertClip(track, asset, 50, 100, 149);
    AddTransition addCmd(track, a, b, 6, 4); // b's incoming overlap: [46, 56)
    Model linked = model;

    // Inside [46, 56) but also strictly inside b's own SHRUNK span
    // (50, 100) once the strip below runs -- 50 itself is the shrunk
    // span's own boundary, not strictly inside it.
    SplitClip cmd(b, 52);
    REQUIRE(cmd.apply(model));
    CHECK_FALSE(model.hasTransition(addCmd.transitionId()));
    CHECK(model.clip(a).out == 49); // a's extension is undone along with the transition
    CHECK(model.check().empty());

    cmd.revert(model);
    CHECK(equalIgnoringIdAllocator(model, linked));
}

TEST_CASE("SplitClip repoints a preserved outgoing transition to the right half (audit C4)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId a = model.insertClip(track, asset, 0, 0, 99);     // [0,100)
    ClipId b = model.insertClip(track, asset, 100, 50, 149); // [100,200), head handle to spare
    AddTransition addCmd(track, a, b, 6, 4);                  // a's outgoing overlap: [96, 106)
    REQUIRE(addCmd.apply(model));

    // a's extended end() is 106 (99 + extendA 6 + 1), transition length
    // is 10 (extendA 6 + extendB 4), so its outgoing overlap is
    // [96, 106). 50 is well before that -- must preserve the dissolve,
    // repointed to the right half (which keeps a's original,
    // still-extended `out` of 105).
    SplitClip cmd(a, 50);
    REQUIRE(cmd.apply(model));
    ClipId right = cmd.rightId();
    REQUIRE(model.hasTransition(addCmd.transitionId()));
    const Transition &t = model.transition(addCmd.transitionId());
    CHECK(t.a == right); // repointed away from the left half...
    CHECK(t.a != a);      // ...which no longer reaches the tail edge at all
    CHECK(model.clip(right).out == 105);
    CHECK(model.clip(a).out == 49); // left half ends cleanly at the split point
    CHECK(model.check().empty());

    cmd.revert(model);
    REQUIRE(model.hasTransition(addCmd.transitionId()));
    CHECK(model.transition(addCmd.transitionId()).a == a); // repointed back before the right half was removed
    CHECK_FALSE(model.hasClip(right));
    CHECK(model.clip(a).out == 105);
}

TEST_CASE("SplitClip strips an outgoing transition when the split point falls inside its own overlap (audit C4)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId a = model.insertClip(track, asset, 0, 0, 99);
    ClipId b = model.insertClip(track, asset, 100, 50, 149); // head handle to spare, same as the previous test
    AddTransition addCmd(track, a, b, 6, 4); // a's outgoing overlap: [96, 106) -- see the previous test's own comment
    Model linked = model;

    SplitClip cmd(a, 98); // inside [96, 106) -- ambiguous, must strip
    REQUIRE(cmd.apply(model));
    CHECK_FALSE(model.hasTransition(addCmd.transitionId()));
    CHECK(model.clip(a).out == 97); // split point minus 1, no leftover extension
    CHECK(model.check().empty());

    cmd.revert(model);
    CHECK(equalIgnoringIdAllocator(model, linked));
}

TEST_CASE("SplitClip refuses and restores the transition when the point only falls inside the extended span "
         "(audit T1)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId a = model.insertClip(track, asset, 0, 0, 49);
    ClipId b = model.insertClip(track, asset, 50, 100, 149);
    AddTransition addCmd(track, a, b, 6, 4); // b's position pulls back from 50 to 46
    REQUIRE(addCmd.apply(model));
    Model linked = model;

    // 48 is inside the transition-extended span [46,100) but not the
    // clip's own base span [50,100) -- must refuse cleanly, not assert
    // inside Model::splitClip on an out-of-range point.
    CHECK_FALSE(SplitClip(b, 48).apply(model));
    CHECK(equalIgnoringIdAllocator(model, linked)); // fully unwound, transition intact
}

TEST_CASE("RemoveTrack strips its own transitions first and restores everything on revert (audit T1)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    TrackId other = model.addTrack(Track::Kind::Video, 1, "V2"); // keep >1 track so removal isn't refused
    (void)other;
    AssetId asset = addTestAsset(model);
    ClipId a = model.insertClip(track, asset, 0, 0, 49);
    ClipId b = model.insertClip(track, asset, 50, 100, 149);
    AddTransition addCmd(track, a, b, 6, 4);
    REQUIRE(addCmd.apply(model));
    Model linked = model;

    RemoveTrack cmd(track);
    REQUIRE(cmd.apply(model));
    CHECK_FALSE(model.hasTrack(track));
    CHECK_FALSE(model.hasTransition(addCmd.transitionId()));
    CHECK(model.check().empty());

    cmd.revert(model);
    CHECK(equalIgnoringIdAllocator(model, linked));
}

// --- T2 (2026-09-22 audit): a clip linked on both sides at once ------------

TEST_CASE("AddTransition refuses a combined overlap that would exceed a shared clip's own length (audit T2)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId a = model.insertClip(track, asset, 0, 0, 99);     // [0,100), length 100
    ClipId b = model.insertClip(track, asset, 100, 0, 19);   // [100,120), length 20 -- the shared middle clip
    ClipId c = model.insertClip(track, asset, 120, 50, 149); // [120,220), length 100, head handle to spare

    // First dissolve absorbs its whole 15-frame overlap from a's tail
    // handle (extendA=15, extendB=0) -- b's own geometry stays untouched
    // (base length still 20).
    REQUIRE(AddTransition(track, a, b, 15, 0).apply(model));
    Model afterFirst = model;

    // Second dissolve tries to absorb its whole 15-frame overlap from
    // c's head handle (extendA=0, extendB=15) -- on its own this also
    // passes the single-transition check (b's still-20-frame length
    // comfortably covers a 15-frame bite), but combined with the first
    // transition's own 15-frame bite out of the SAME clip b, the two
    // overlaps would together consume 30 frames of a clip that's only 20
    // long -- exactly the audit's repro (each passes in isolation, the
    // combination corrupts EngineSync::planTrackSegments's layout).
    CHECK_FALSE(AddTransition(track, b, c, 0, 15).apply(model));
    CHECK(equalIgnoringIdAllocator(model, afterFirst)); // the refused attempt left no trace

    // A second dissolve that fits within b's remaining budget (15 already
    // spent by the first + 5 here == its 20-frame length, exactly) is
    // still allowed.
    REQUIRE(AddTransition(track, b, c, 0, 5).apply(model));
    CHECK(model.check().empty());
}

TEST_CASE("Model::check() flags a clip with combined transition overlap longer than its own length (audit T2)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    ClipId a = model.insertClip(track, asset, 0, 0, 99);
    ClipId b = model.insertClip(track, asset, 100, 0, 19); // length 20
    ClipId c = model.insertClip(track, asset, 120, 0, 99);

    // Built directly against Model, bypassing AddTransition's own new
    // refusal (doc 04: "Model doesn't refuse, it asserts on programmer
    // error" -- callers are expected to validate), to test check()'s new
    // invariant in isolation from the command-level fix above.
    model.addTransition(track, a, b, 15, 0);
    model.addTransition(track, b, c, 0, 15);

    std::vector<std::string> problems = model.check();
    bool found = false;
    for (const auto &p : problems) {
        if (p.find("combined transition overlap") != std::string::npos)
            found = true;
    }
    CHECK(found);
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
    ClipId clip2 = model.insertClip(locked, asset, 100, 0, 99); // adjacent to `clip`
    CHECK_FALSE(AddTransition(locked, clip, clip2, 5, 0).apply(model));
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

TEST_CASE("UndoStack: markDirty forces isClean() false even on an otherwise-clean empty stack (audit A2)")
{
    Model model = Model::createEmpty();
    UndoStack undoStack(model);

    // clear() alone makes the empty stack the clean state, so it reports
    // clean by definition -- exactly the trap recovery fell into: an empty
    // stack looks identical to a freshly-saved one.
    undoStack.clear();
    CHECK(undoStack.isClean());

    undoStack.markDirty();
    CHECK_FALSE(undoStack.isClean());
    // Still dirty even though nothing has been executed since -- no real
    // undo-stack depth can accidentally satisfy the sentinel.
    CHECK_FALSE(undoStack.canUndo());
    CHECK_FALSE(undoStack.isClean());

    undoStack.setCleanPoint();
    CHECK(undoStack.isClean());
}

TEST_CASE("UndoStack: a new edit at the saved depth is not clean")
{
    // Depth alone can't tell these apart: undo below the save point, edit,
    // and the stack is back at the saved depth with other content.
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    UndoStack undoStack(model);

    REQUIRE(undoStack.execute(std::make_unique<InsertClip>(track, asset, 0, 0, 99)));
    undoStack.setCleanPoint();
    CHECK(undoStack.undo());
    REQUIRE(undoStack.execute(std::make_unique<InsertClip>(track, asset, 200, 0, 99)));
    CHECK_FALSE(undoStack.isClean());
}

TEST_CASE("UndoStack: an async save marks the state it captured, not the current one (doc 19 MT1)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    UndoStack undoStack(model);

    REQUIRE(undoStack.execute(std::make_unique<InsertClip>(track, asset, 0, 0, 99)));
    UndoStack::State submitted = undoStack.state(); // save submitted here
    REQUIRE(undoStack.execute(std::make_unique<InsertClip>(track, asset, 200, 0, 99)));

    undoStack.setCleanPoint(submitted); // ... and completes after the edit
    CHECK_FALSE(undoStack.isClean());   // the file doesn't have the second clip
    CHECK(undoStack.undo());
    CHECK(undoStack.isClean()); // back to what the file holds

    // Cleared (Open/New) while a save of the old project was in flight:
    // its completion can't make the new project clean or dirty by accident.
    undoStack.clear();
    REQUIRE(undoStack.execute(std::make_unique<InsertClip>(track, asset, 400, 0, 99)));
    undoStack.setCleanPoint(submitted);
    CHECK_FALSE(undoStack.isClean());
    CHECK(undoStack.undo());
    CHECK_FALSE(undoStack.isClean());
}

TEST_CASE("UndoStack: a merge into the captured entry makes it a new state")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Audio, 0, "A1");
    UndoStack undoStack(model);

    REQUIRE(undoStack.execute(std::make_unique<SetTrackVolume>(track, 0.5)));
    REQUIRE(undoStack.execute(std::make_unique<SetTrackVolume>(track, 0.6)));
    UndoStack::State submitted = undoStack.state();
    // The same drag continues and merges into the top entry.
    REQUIRE(undoStack.execute(std::make_unique<SetTrackVolume>(track, 0.7)));
    undoStack.setCleanPoint(submitted);
    CHECK_FALSE(undoStack.isClean());
}

TEST_CASE("UndoStack: the clean state survives the oldest entries being dropped")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model);
    UndoStack undoStack(model);
    undoStack.limit = 2;

    REQUIRE(undoStack.execute(std::make_unique<InsertClip>(track, asset, 0, 0, 9)));
    undoStack.setCleanPoint();
    REQUIRE(undoStack.execute(std::make_unique<InsertClip>(track, asset, 100, 0, 9)));
    REQUIRE(undoStack.execute(std::make_unique<InsertClip>(track, asset, 200, 0, 9))); // drops the saved entry
    CHECK_FALSE(undoStack.isClean());
    CHECK(undoStack.undo());
    CHECK(undoStack.undo()); // the bottom of the stack is now the saved state
    CHECK(undoStack.isClean());
}

TEST_CASE("UndoStack: setCleanPoint and markDirty both emit changed (audit A1/A2)")
{
    Model model = Model::createEmpty();
    UndoStack undoStack(model);
    int changedCount = 0;
    undoStack.changed.connect([&] { ++changedCount; });

    undoStack.setCleanPoint();
    CHECK(changedCount == 1);

    undoStack.markDirty();
    CHECK(changedCount == 2);
}

TEST_CASE("UndoStack: a new drag right after a save is never merged into the just-saved entry (audit C2)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Audio, 0, "A1");
    UndoStack undoStack(model);

    // First "drag": one push, since the stack starts empty and there's
    // nothing yet to merge into.
    REQUIRE(undoStack.execute(std::make_unique<SetTrackVolume>(track, 0.5)));
    undoStack.setCleanPoint(); // simulates a Save right after this drag

    // A second, later drag on the SAME track: SetTrackVolume::mergeWith
    // only checks "same track" (see the dedicated merge test above), so
    // without the fix this would merge straight into the entry that was
    // just marked clean, leaving the stack size (and isClean()) unchanged.
    REQUIRE(undoStack.execute(std::make_unique<SetTrackVolume>(track, 0.9)));
    CHECK_FALSE(undoStack.isClean());

    // The saved value must still be recoverable by undoing exactly once:
    // if the second drag had wrongly merged into the saved entry, one
    // undo would jump all the way back to the value before EITHER drag.
    CHECK(undoStack.undo());
    CHECK(model.track(track).volume == doctest::Approx(0.5));
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

    // tests/common/random_commands.h, shared with tests/engine's "verify()
    // never fails across 500 undoable commands" -- the same seed, so that
    // test runs the first 500 commands of this stream.
    int staleSnapshots = 0;
    int appliedCount =
        ustudio::testing::runRandomCommands(model, undoStack, track, asset, 2026, 10'000, {}, &staleSnapshots);
    CHECK(staleSnapshots == 0);

    REQUIRE(appliedCount > 0);
    for (int i = 0; i < appliedCount; ++i) {
        (void)model.snapshot();
        REQUIRE(undoStack.undo());
        if (*model.snapshot() != model.project())
            ++staleSnapshots; // undo must invalidate the cached snapshot too
    }
    CHECK(staleSnapshots == 0);

    CHECK_FALSE(undoStack.canUndo());
    CHECK(equalIgnoringIdAllocator(model, snapshot));
}

TEST_CASE("SetClipAsset: another asset for one clip, keeping its place and transform; exact revert")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId title = addTestAsset(model, 1000);
    ClipId clip = model.insertClip(track, title, 50, 10, 99);
    ClipId other = model.insertClip(track, title, 200, 0, 99);
    model.setClipSourceParams(clip, {{"field.name", std::string("Ada"), {}}});
    Transform moved;
    moved.flipH = true;
    model.setClipTransform(clip, moved);

    AssetId baked = addTestAsset(model, 100); // frames 0..99: the clip's 10..99 fit
    const Model withBaked = model;
    SetClipAsset swap(clip, baked, {}, "Bake title");
    CHECK(swap.label() == "Bake title");
    REQUIRE(swap.apply(model));
    CHECK(model.clip(clip).asset == baked);
    CHECK(model.clip(clip).sourceParams.empty());
    CHECK(model.clip(clip).position == 50);
    CHECK(model.clip(clip).in == 10);
    CHECK(model.clip(clip).out == 99);
    CHECK(model.clip(clip).transform.get().flipH);
    CHECK(model.clip(other).asset == title); // only this clip
    CHECK(model.check().empty());
    swap.revert(model);
    CHECK(model == withBaked);

    // Too short for the clip's range, a locked track, unknown ids: refused.
    AssetId shorter = addTestAsset(model, 99);
    const Model untouched = model;
    CHECK_FALSE(SetClipAsset(clip, shorter, {}).apply(model));
    CHECK_FALSE(SetClipAsset(clip, AssetId{9999}, {}).apply(model));
    CHECK_FALSE(SetClipAsset(ClipId{9999}, baked, {}).apply(model));
    model.setTrackFlags(track, false, false, true);
    CHECK_FALSE(SetClipAsset(clip, baked, {}).apply(model));
    model.setTrackFlags(track, false, false, false);
    CHECK(model == untouched);
}
