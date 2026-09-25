// Model's precondition checks in a release (NDEBUG) build: this binary
// compiles core itself with -DNDEBUG (tests/core/meson.build), so the
// asserts are gone and what's left must be safe -- a logged error and a
// no-op or a placeholder, never an end() iterator dereferenced or erased.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/model/model.h"

using namespace ustudio::core;

TEST_CASE("Model in a release build: unknown ids are logged no-ops, not undefined behaviour")
{
#ifndef NDEBUG
    FAIL("this test must be built with NDEBUG");
#endif
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset asset;
    asset.path = "color:red";
    asset.info.hasVideo = true;
    asset.info.lengthInSequenceFrames = 100;
    AssetId assetId = model.addAsset(asset);
    ClipId clipId = model.insertClip(track, assetId, 0, 0, 9);
    const Project before = model.project();

    const AssetId noAsset{999};
    const TrackId noTrack{998};
    const ClipId noClip{997};
    const MarkerId noMarker{996};
    const TransitionId noTransition{995};

    model.removeAsset(noAsset);
    model.extendAssetLength(noAsset, 500);
    model.setAssetLength(noAsset, 500);
    model.removeTrack(noTrack);
    model.moveTrack(noTrack, 0);
    model.removeMarker(noMarker);
    model.setMarker(noMarker, 5, "x");
    model.retargetTransitionClip(noTransition, clipId, clipId);
    CHECK(model.splitClip(noClip, 5) == ClipId{});
    CHECK(model.splitClip(clipId, 0) == ClipId{}); // not strictly inside
    CHECK(model.project() == before);

    // Accessors hand back a placeholder instead.
    CHECK(model.asset(noAsset).path.empty());
    CHECK(model.clip(noClip).asset == AssetId{});
    CHECK(model.track(noTrack).clips.empty());
    CHECK(model.transition(noTransition).length == 0);
    CHECK(model.marker(noMarker).text.empty());
    CHECK(model.check().empty());
}

TEST_CASE("Model in a release build: a project without its active sequence doesn't crash")
{
    Project project;
    project.activeSequence = SequenceId{42};
    Model model(project);
    CHECK(model.sequence().clips.empty());
}
