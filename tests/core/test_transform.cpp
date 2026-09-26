// ADR-018: clip transform arithmetic, check() rules, the command's gesture
// merge, and the transform surviving edits.

#include "doctest.h"

#include "core/commands/primitives.h"
#include "core/commands/timeline_edits.h"
#include "core/commands/undo_stack.h"
#include "core/model/model.h"
#include "core/model/transform.h"

#include <cmath>

using namespace ustudio::core;

namespace {

Profile hd()
{
    Profile p; // 1920x1080, 16:9
    return p;
}

} // namespace

TEST_CASE("transform: a 1344x768 picture is fitted and centred in 1080p by default")
{
    const Placement p = placementFor(Transform{}, 1344, 768, hd());
    CHECK(p.cx == doctest::Approx(960));
    CHECK(p.cy == doctest::Approx(540));
    CHECK(p.h == doctest::Approx(1080));
    CHECK(p.w == doctest::Approx(1890)); // 1344 * 1080/768
    // The track compositor scales a picture of the frame's aspect by itself;
    // another aspect is fitted by the affine filter.
    CHECK_FALSE(isIdentity(Transform{}, 1344, 768, hd()));
    CHECK(transformFilters(Transform{}, 1344, 768, hd()).back().service == "affine");
    CHECK(isIdentity(Transform{}, 1280, 720, hd()));
    CHECK(isIdentity(Transform{}, 1920, 1080, hd()));
    CHECK(transformFilters(Transform{}, 1280, 720, hd()).empty());
    Transform rotated;
    rotated.rotation.value = 1;
    CHECK_FALSE(isIdentity(rotated, 1920, 1080, hd()));
    CHECK(transformFilters(rotated, 1920, 1080, hd()).back().service == "affine");
    // 4:3 is pillarboxed, centred.
    const Placement four = placementFor(Transform{}, 1440, 1080, hd());
    CHECK(four.w == doctest::Approx(1440));
    CHECK(four.cx == doctest::Approx(960));
}

TEST_CASE("transform: stretch, explicit placement, crop and the switch to explicit")
{
    Transform t;
    t.bounds = Transform::Bounds::Stretch;
    CHECK(placementFor(t, 640, 480, hd()) == Placement{960, 540, 1920, 1080, 0});

    // Cropping 336 of 1344 pixels leaves a 1008x768 picture (4:3-ish) to fit.
    Transform cropped;
    cropped.cropLeft.value = 336;
    CHECK(croppedWidth(cropped, 1344) == doctest::Approx(1008));
    CHECK(placementFor(cropped, 1344, 768, hd()).w == doctest::Approx(1008 * 1080.0 / 768));
    CHECK_FALSE(isIdentity(cropped, 1920, 1080, hd()));

    // Moving a fitted picture starts from where it already is.
    Transform fitted;
    fitted.rotation.value = 15;
    const Transform placed = explicitTransform(fitted, 1344, 768, hd());
    CHECK(placed.bounds == Transform::Bounds::None);
    CHECK(placementFor(placed, 1344, 768, hd()) == placementFor(fitted, 1344, 768, hd()));
}

TEST_CASE("transform: check() refuses sizes, crops and values that make no sense")
{
    Transform t;
    CHECK(transformProblem(t).empty());
    t.bounds = Transform::Bounds::None; // explicit, zero size
    CHECK_FALSE(transformProblem(t).empty());
    t.width.value = 100;
    t.height.value = 50;
    CHECK(transformProblem(t).empty());
    t.cropTop.value = -1;
    CHECK_FALSE(transformProblem(t).empty());
    t.cropTop.value = 0;
    t.rotation.value = std::nan("");
    CHECK_FALSE(transformProblem(t).empty());
}

TEST_CASE("SetClipTransform: a drag is one undo step; the next gesture is another")
{
    Model model = Model::createEmpty();
    const TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset asset;
    asset.path = "color:red";
    asset.info.hasVideo = true;
    asset.info.lengthInSequenceFrames = 100;
    const ClipId clip = model.insertClip(track, model.addAsset(asset), 0, 0, 99);
    UndoStack undo(model);
    const Model before = model;

    Transform t = explicitTransform(Transform{}, 1920, 1080, hd());
    for (int step = 1; step <= 5; ++step) { // one drag: five pointer moves
        t.x.value = 960 + step * 10;
        REQUIRE(undo.execute(std::make_unique<SetClipTransform>(clip, t, 42)));
    }
    CHECK(model.clip(clip).transform.get().x.value == doctest::Approx(1010));
    REQUIRE(undo.undo());
    CHECK(model == before); // the whole drag at once
    CHECK_FALSE(undo.canUndo());
    REQUIRE(undo.redo());
    CHECK(model.clip(clip).transform.get().x.value == doctest::Approx(1010));

    t.rotation.value = 90;
    REQUIRE(undo.execute(std::make_unique<SetClipTransform>(clip, t, 43))); // another gesture
    REQUIRE(undo.undo());
    CHECK(model.clip(clip).transform.get().rotation.value == doctest::Approx(0));

    // Refused: an invalid transform leaves the model alone.
    Transform bad = t;
    bad.width.value = 0;
    CHECK_FALSE(undo.execute(std::make_unique<SetClipTransform>(clip, bad)));
    CHECK(model.check().empty());
}

TEST_CASE("transform: a split gives both halves the clip's transform")
{
    Model model = Model::createEmpty();
    const TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset asset;
    asset.path = "color:red";
    asset.info.hasVideo = true;
    asset.info.lengthInSequenceFrames = 100;
    const ClipId clip = model.insertClip(track, model.addAsset(asset), 0, 0, 99);
    Transform t = explicitTransform(Transform{}, 1920, 1080, hd());
    t.rotation.value = 30;
    t.flipH = true;
    model.setClipTransform(clip, t);
    UndoStack undo(model);
    REQUIRE(undo.execute(std::make_unique<SplitClip>(clip, 50)));
    for (ClipId id : model.track(track).clips)
        CHECK(model.clip(id).transform.get() == t);
    CHECK(model.check().empty());
}

TEST_CASE("transform: survives trim, ripple, copy, a frame-rate change and save/load")
{
    Model model = Model::createEmpty();
    const TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset asset;
    asset.path = "color:red";
    asset.info.hasVideo = true;
    asset.info.lengthInSequenceFrames = 300;
    const AssetId id = model.addAsset(asset);
    const ClipId first = model.insertClip(track, id, 0, 0, 99);
    const ClipId second = model.insertClip(track, id, 100, 0, 99);
    Transform t = explicitTransform(Transform{}, 1920, 1080, hd());
    t.x.value = 300;
    t.rotation.value = -20;
    t.cropTop.value = 40;
    t.flipV = true;
    model.setClipTransform(second, t);
    UndoStack undo(model);

    REQUIRE(undo.execute(std::make_unique<ResizeClip>(second, 10, 99, 110))); // trim its start
    CHECK(model.clip(second).transform.get() == t);
    REQUIRE(undo.execute(std::make_unique<RippleDelete>(first))); // it moves left
    CHECK(model.clip(second).position == 10);                     // 110 after the trim, less the 100 frames removed
    CHECK(model.clip(second).transform.get() == t);
    REQUIRE(undo.execute(std::make_unique<CopyClip>(second, track, 500)));
    for (ClipId clip : model.track(track).clips)
        CHECK(model.clip(clip).transform.get() == t);
    REQUIRE(undo.execute(std::make_unique<ChangeSequenceFrameRate>(Rational{60, 1})));
    for (ClipId clip : model.track(track).clips)
        CHECK(model.clip(clip).transform.get() == t); // project pixels: a rate change doesn't touch it
    CHECK(model.check().empty());
}

TEST_CASE("transform: a timeline is transformed once a clip has a non-default transform")
{
    Model model = Model::createEmpty();
    const TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset asset;
    asset.path = "color:red";
    asset.info.hasVideo = true;
    asset.info.width = 1344; // another aspect: its default Fit still doesn't count
    asset.info.height = 768;
    asset.info.lengthInSequenceFrames = 300;
    const ClipId clip = model.insertClip(track, model.addAsset(asset), 0, 0, 99);
    CHECK_FALSE(hasTransformedClip(*model.snapshot()));
    Transform flipped;
    flipped.flipH = true;
    model.setClipTransform(clip, flipped);
    CHECK(hasTransformedClip(*model.snapshot()));
    model.setClipTransform(clip, Transform{});
    CHECK_FALSE(hasTransformedClip(*model.snapshot()));
}
