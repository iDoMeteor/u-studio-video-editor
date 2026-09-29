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

TEST_CASE("transform: keyframed values ease to a static transform at a frame, and handles key at the frame")
{
    Transform t;
    t.bounds = Transform::Bounds::None;
    t.x.keyframes = {{0, 100, Easing::Linear}, {20, 300, Easing::Linear}};
    t.y.value = 540;
    t.width.value = 960;
    t.height.value = 540;
    t.rotation.keyframes = {{10, 0, Easing::Linear}, {30, 90, Easing::Linear}};

    const Transform mid = transformAt(t, 10);
    CHECK(mid.x.value == doctest::Approx(200));
    CHECK(mid.x.keyframes.empty());
    CHECK(mid.rotation.value == 0);
    CHECK(mid.y.value == 540);
    CHECK(transformAt(t, -5).x.value == 100); // holds outside the keys
    CHECK(transformAt(t, 99).rotation.value == 90);
    const Placement p = placementAt(t, 20, 1920, 1080, hd());
    CHECK(p.cx == doctest::Approx(300));
    CHECK(p.rotation == doctest::Approx(45));
    Transform still;
    still.rotation.value = 5;
    CHECK(transformAt(still, 7) == still);

    // Dragging at frame 10: x gains a key there, y (static) just changes,
    // and rotation's key at 10 keeps its easing.
    t.rotation.keyframes.front().easing = Easing::CubicIn;
    Transform edited = mid;
    edited.x.value = 250;
    edited.y.value = 600;
    edited.rotation.value = 15;
    const Transform out = withTransformAt(t, 10, edited);
    REQUIRE(out.x.keyframes.size() == 3);
    CHECK(out.x.keyframes[1] == Keyframe{10, 250, Easing::Linear});
    CHECK(out.y.value == 600);
    CHECK(out.y.keyframes.empty());
    REQUIRE(out.rotation.keyframes.size() == 2);
    CHECK(out.rotation.keyframes[0] == Keyframe{10, 15, Easing::CubicIn});
    CHECK(transformAt(out, 10) == edited);
}

TEST_CASE("transform: keyframes become affine's animated rect and rotation for each cut")
{
    Transform t;
    t.bounds = Transform::Bounds::None;
    t.x.keyframes = {{0, 480, Easing::Linear}, {40, 1440, Easing::SmoothNatural}};
    t.y.value = 270;
    t.width.keyframes = {{0, 960, Easing::Linear}, {40, 480, Easing::SmoothNatural}};
    t.height.value = 540;
    t.rotation.keyframes = {{10, 0, Easing::Linear}, {30, 90, Easing::Linear}};
    auto property = [](const std::vector<NativeFilter> &filters, const std::string &name) {
        for (const NativeFilter &f : filters)
            for (const auto &[key, value] : f.properties)
                if (key == name)
                    return value;
        return std::string();
    };

    // x and width share their keys: one rect key each, with their easing.
    const auto whole = transformFilters(t, 1920, 1080, hd(), 1.0, 1.0, 0, 60);
    CHECK(property(whole, "transition.rect") == "0=0 0 960 540 1;40$=1200 0 480 540 1");
    CHECK(property(whole, "transition.fix_rotate_x") == "10=0;30=90");
    // A cut 20 frames in, 20 long, at half preview scale: its edges carry
    // the values there (frame 20: x 960, width 720; frame 39: x 1416,
    // width 492), halved.
    const auto cut = transformFilters(t, 1920, 1080, hd(), 0.5, 1.0, 20, 20);
    CHECK(property(cut, "transition.rect") == "0=300 0 360 270 1;19=585 0 246 270 1");
    CHECK(property(cut, "transition.fix_rotate_x") == "0=45;10=90");

    // Keys on different frames: every frame from the first key to the last.
    Transform apart = t;
    apart.width.keyframes = {{5, 960, Easing::Linear}, {15, 480, Easing::Linear}};
    const std::string sampled = property(transformFilters(apart, 1920, 1080, hd(), 1.0, 1.0, 0, 60), "transition.rect");
    CHECK(std::count(sampled.begin(), sampled.end(), ';') == 40); // frames 0-40
    CHECK(sampled.starts_with("0=0 0 960 540 1;1=24 0 960 540 1;"));

    // The GPU's movit.rect: affine's opacity dropped from every key.
    Transform level = t;
    level.rotation = {};
    CHECK(property(gpuTransformFilters(level, 1920, 1080, hd(), 1.0, 1.0, 0, 60), "rect") ==
          "0=0 0 960 540;40$=1200 0 480 540");
    // movit's flips act on the frame movit.rect has placed, so the rect is
    // mirrored with them, key for key.
    Transform flipped = level;
    flipped.flipH = true;
    flipped.flipV = true;
    CHECK(property(gpuTransformFilters(flipped, 1920, 1080, hd(), 1.0, 1.0, 0, 60), "rect") ==
          "0=960 540 960 540;40$=240 540 480 540");
    CHECK(property(gpuTransformFilters(flipped, 1920, 1080, hd(), 0.5, 1.0, 0, 60), "rect") ==
          "0=480 270 480 270;40$=120 270 240 270");
    // Not animated: never an identity or a plain compositor fit.
    Transform turning;
    turning.bounds = Transform::Bounds::None;
    turning.x.value = 960;
    turning.y.value = 540;
    turning.width.value = 1920;
    turning.height.value = 1080;
    CHECK(transformFilters(turning, 1920, 1080, hd()).size() == 1);
    turning.bounds = Transform::Bounds::Fit;
    CHECK(transformFilters(turning, 1920, 1080, hd()).empty());
    turning.rotation.keyframes = {{0, 0, Easing::Linear}};
    CHECK_FALSE(isIdentity(turning, 1920, 1080, hd()));
    CHECK_FALSE(compositorFits(turning));
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
    t.rotation.value = 0;

    // Keyframes: placed pictures only, never crops, in order, sizes positive.
    t.x.keyframes = {{0, 100, Easing::Linear}, {10, 200, Easing::Linear}};
    CHECK(transformProblem(t).empty());
    t.bounds = Transform::Bounds::Fit;
    CHECK_FALSE(transformProblem(t).empty());
    t.bounds = Transform::Bounds::None;
    t.cropLeft.keyframes = {{0, 10, Easing::Linear}};
    CHECK_FALSE(transformProblem(t).empty());
    t.cropLeft.keyframes.clear();
    t.x.keyframes = {{10, 100, Easing::Linear}, {10, 200, Easing::Linear}};
    CHECK_FALSE(transformProblem(t).empty());
    t.x.keyframes.clear();
    t.width.keyframes = {{0, 100, Easing::Linear}, {10, 0, Easing::Linear}};
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
