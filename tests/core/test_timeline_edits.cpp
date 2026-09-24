#include "doctest.h"

#include "core/commands/composite_command.h"
#include "core/commands/timeline_edits.h"
#include "core/commands/undo_stack.h"
#include "core/model/model.h"

using namespace ustudio::core;

namespace {

AssetId addAsset(Model &model, FrameIndex length = 1000)
{
    Asset asset;
    asset.displayName = "clip.mp4";
    asset.info.hasVideo = true;
    asset.info.hasAudio = true;
    asset.info.lengthInSequenceFrames = length;
    return model.addAsset(asset);
}

// See test_commands.cpp: undo never rolls back the id allocator.
bool equalIgnoringIdAllocator(const Model &a, const Model &b)
{
    Project pa = a.project();
    Project pb = b.project();
    pa.nextId = pb.nextId = 0;
    return pa == pb;
}

// Every edit here must leave the model invariant-clean after apply and
// after revert, and revert must restore it exactly.
void checkRoundTrip(Model &model, Command &command, const Model &before)
{
    CHECK(model.check().empty());
    command.revert(model);
    CHECK(model.check().empty());
    CHECK(equalIgnoringIdAllocator(model, before));
}

// V1: [a 0..100) [b 100..200) with a 10-frame dissolve, gap, [c 300..400)
// [d 400..500) with a 10-frame dissolve.
struct Fixture
{
    Model model = Model::createEmpty();
    TrackId track;
    AssetId asset;
    ClipId a, b, c, d;
    TransitionId ab, cd;
    Fixture()
    {
        track = model.addTrack(Track::Kind::Video, 0, "V1");
        asset = addAsset(model);
        a = model.insertClip(track, asset, 0, 100, 199);
        b = model.insertClip(track, asset, 100, 300, 399);
        c = model.insertClip(track, asset, 300, 500, 599);
        d = model.insertClip(track, asset, 400, 700, 799);
        ab = model.addTransition(track, a, b, 5, 5);
        cd = model.addTransition(track, c, d, 5, 5);
        REQUIRE(model.check().empty());
    }
};

} // namespace

TEST_CASE("ShiftClips moves the later group together and keeps the dissolve inside it")
{
    Fixture f;
    Model before = f.model;
    ShiftClips shift(f.track, 250, -60); // c and d move left 60, into the gap
    REQUIRE(shift.apply(f.model));
    CHECK(f.model.clip(f.c).position == before.clip(f.c).position - 60);
    CHECK(f.model.clip(f.d).position == before.clip(f.d).position - 60);
    CHECK(f.model.hasTransition(f.cd));
    CHECK(f.model.clip(f.a).position == before.clip(f.a).position);
    checkRoundTrip(f.model, shift, before);
}

TEST_CASE("ShiftClips refuses to tear a dissolve apart, collide, or go below frame 0")
{
    Fixture f;
    Model before = f.model;
    // `from` between b's start and its partner a: a stays, b would move.
    CHECK_FALSE(ShiftClips(f.track, f.model.clip(f.b).position, 10).apply(f.model));
    // Moving c/d left past the end of b.
    CHECK_FALSE(ShiftClips(f.track, 250, -150).apply(f.model));
    // Everything left past frame 0.
    CHECK_FALSE(ShiftClips(f.track, 0, -1).apply(f.model));
    // Nothing after `from`.
    CHECK_FALSE(ShiftClips(f.track, 10'000, 5).apply(f.model));
    CHECK(equalIgnoringIdAllocator(f.model, before));
}

TEST_CASE("Close Gap as ShiftClips keeps the dissolves after the gap (they used to be stripped)")
{
    Fixture f;
    Model before = f.model;
    const FrameIndex gapStart = f.model.clip(f.b).end();
    const FrameIndex gapEnd = f.model.clip(f.c).position;
    std::vector<std::unique_ptr<Command>> steps;
    steps.push_back(std::make_unique<ShiftClips>(f.track, gapEnd, -(gapEnd - gapStart)));
    CompositeCommand closeGap("Close gap", std::move(steps));
    REQUIRE(closeGap.apply(f.model));
    CHECK(f.model.clip(f.c).position == gapStart);
    CHECK(f.model.hasTransition(f.cd));
    CHECK(f.model.sequence().transitions.size() == 2);
    checkRoundTrip(f.model, closeGap, before);
}

TEST_CASE("RippleDelete removes the clip and pulls everything after it left by its length")
{
    Fixture f;
    Model before = f.model;
    // c has a dissolve into d: it goes with c; d returns to its base
    // position and then shifts left by c's base length.
    // c: base [300, 400) (the dissolve extended its out by 5); d starts at
    // 395 extended, 400 base. Ripple-deleting c puts d at c's start.
    const FrameIndex cBaseStart = before.clip(f.c).position;
    RippleDelete ripple(f.c);
    REQUIRE(ripple.apply(f.model));
    CHECK_FALSE(f.model.hasClip(f.c));
    CHECK_FALSE(f.model.hasTransition(f.cd));
    CHECK(f.model.clip(f.d).position == cBaseStart);
    CHECK(f.model.clip(f.d).in == 700); // base in: the dissolve's head extension is gone
    CHECK(f.model.hasTransition(f.ab)); // untouched dissolve before it
    checkRoundTrip(f.model, ripple, before);
}

TEST_CASE("RippleDelete of the last clip is a plain delete")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addAsset(model);
    model.insertClip(track, asset, 0, 0, 99);
    ClipId last = model.insertClip(track, asset, 200, 0, 99);
    Model before = model;
    RippleDelete ripple(last);
    REQUIRE(ripple.apply(model));
    CHECK_FALSE(model.hasClip(last));
    checkRoundTrip(model, ripple, before);
}

TEST_CASE("RippleTrim on the tail moves later clips by the same amount, both ways")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addAsset(model);
    ClipId x = model.insertClip(track, asset, 0, 0, 99);
    ClipId y = model.insertClip(track, asset, 100, 0, 99);
    ClipId z = model.insertClip(track, asset, 300, 0, 99);
    Model before = model;

    SUBCASE("extend")
    {
        RippleTrim trim(x, RippleTrim::Edge::Tail, 20);
        REQUIRE(trim.apply(model));
        CHECK(model.clip(x).out == 119);
        CHECK(model.clip(y).position == 120);
        CHECK(model.clip(z).position == 320);
        checkRoundTrip(model, trim, before);
    }
    SUBCASE("shorten")
    {
        RippleTrim trim(x, RippleTrim::Edge::Tail, -30);
        REQUIRE(trim.apply(model));
        CHECK(model.clip(x).out == 69);
        CHECK(model.clip(y).position == 70);
        CHECK(model.clip(z).position == 270);
        checkRoundTrip(model, trim, before);
    }
}

TEST_CASE("RippleTrim on the head keeps the clip's start and moves later clips")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addAsset(model);
    ClipId x = model.insertClip(track, asset, 50, 100, 199);
    ClipId y = model.insertClip(track, asset, 150, 0, 99);
    Model before = model;

    SUBCASE("trim the front off")
    {
        RippleTrim trim(x, RippleTrim::Edge::Head, 30);
        REQUIRE(trim.apply(model));
        CHECK(model.clip(x).position == 50);
        CHECK(model.clip(x).in == 130);
        CHECK(model.clip(y).position == 120);
        checkRoundTrip(model, trim, before);
    }
    SUBCASE("extend the front, into the source's handle")
    {
        RippleTrim trim(x, RippleTrim::Edge::Head, -40);
        REQUIRE(trim.apply(model));
        CHECK(model.clip(x).position == 50);
        CHECK(model.clip(x).in == 60);
        CHECK(model.clip(y).position == 190);
        checkRoundTrip(model, trim, before);
    }
    SUBCASE("refused past the start of the source")
    {
        CHECK_FALSE(RippleTrim(x, RippleTrim::Edge::Head, -101).apply(model));
        CHECK(equalIgnoringIdAllocator(model, before));
    }
}

TEST_CASE("RippleTrim removes only the dissolve on the trimmed edge and keeps later ones")
{
    Fixture f;
    Model before = f.model;
    RippleTrim trim(f.b, RippleTrim::Edge::Tail, 15);
    REQUIRE(trim.apply(f.model));
    CHECK(f.model.hasTransition(f.ab)); // on b's head, untouched
    CHECK(f.model.hasTransition(f.cd)); // after b, moved along with c/d
    CHECK(f.model.clip(f.c).position == before.clip(f.c).position + 15);
    checkRoundTrip(f.model, trim, before);

    RippleTrim headTrim(f.b, RippleTrim::Edge::Head, 10);
    REQUIRE(headTrim.apply(f.model));
    CHECK_FALSE(f.model.hasTransition(f.ab)); // on the trimmed edge
    checkRoundTrip(f.model, headTrim, before);
}

TEST_CASE("SlipClip changes the source window only, and respects the source's bounds")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addAsset(model, 300);
    ClipId x = model.insertClip(track, asset, 10, 100, 199);
    Model before = model;

    SlipClip slip(x, 50);
    REQUIRE(slip.apply(model));
    CHECK(model.clip(x).position == 10);
    CHECK(model.clip(x).length() == 100);
    CHECK(model.clip(x).in == 150);
    CHECK(model.clip(x).out == 249);
    checkRoundTrip(model, slip, before);

    CHECK_FALSE(SlipClip(x, 101).apply(model));  // out would pass the asset's end (300)
    CHECK_FALSE(SlipClip(x, -101).apply(model)); // in would go below 0
    CHECK(equalIgnoringIdAllocator(model, before));
}

TEST_CASE("SlipClip on a clip with dissolves removes them and slips the base window")
{
    Fixture f;
    Model before = f.model;
    SlipClip slip(f.b, 20);
    REQUIRE(slip.apply(f.model));
    CHECK_FALSE(f.model.hasTransition(f.ab));
    CHECK(f.model.clip(f.b).in == 300 + 20);
    checkRoundTrip(f.model, slip, before);
}

TEST_CASE("CopyClip copies name and stream switches, refuses overlap, and undoes cleanly")
{
    Model model = Model::createEmpty();
    TrackId video = model.addTrack(Track::Kind::Video, 0, "V1");
    TrackId audio = model.addTrack(Track::Kind::Audio, 1, "A1");
    AssetId asset = addAsset(model);
    ClipId x = model.insertClip(video, asset, 0, 0, 99);
    model.setClipName(x, "Intro");
    model.setClipEnabled(x, true, false); // video-only (Split Audio's video half)
    Model before = model;

    CopyClip copy(x, video, 200);
    REQUIRE(copy.apply(model));
    const Clip &c = model.clip(copy.copyId());
    CHECK(c.position == 200);
    CHECK(c.in == 0);
    CHECK(c.out == 99);
    CHECK(c.name == "Intro");
    CHECK(c.videoEnabled);
    CHECK_FALSE(c.audioEnabled);
    checkRoundTrip(model, copy, before);

    CHECK_FALSE(CopyClip(x, video, 50).apply(model)); // overlaps x
    CHECK_FALSE(CopyClip(x, audio, 0).apply(model));  // its audio is off: nothing for an audio track
    CHECK(equalIgnoringIdAllocator(model, before));
}

TEST_CASE("Markers: add, edit (merged while dragging), remove, all undoable")
{
    Model model = Model::createEmpty();
    UndoStack undo(model);
    Model before = model;

    auto add = std::make_unique<AddMarker>(100, "Chorus");
    AddMarker *addRaw = add.get();
    REQUIRE(undo.execute(std::move(add)));
    MarkerId id = addRaw->markerId();
    REQUIRE(model.hasMarker(id));
    CHECK(model.marker(id).at == 100);

    // A drag: many edits, one undo step.
    for (FrameIndex at = 101; at <= 110; ++at)
        REQUIRE(undo.execute(std::make_unique<EditMarker>(id, at, "Chorus")));
    CHECK(model.marker(id).at == 110);
    REQUIRE(undo.undo());
    CHECK(model.marker(id).at == 100);
    REQUIRE(undo.redo());

    REQUIRE(undo.execute(std::make_unique<RemoveMarker>(id)));
    CHECK_FALSE(model.hasMarker(id));
    REQUIRE(undo.undo());
    CHECK(model.marker(id).at == 110);
    CHECK(model.check().empty());

    while (undo.canUndo())
        REQUIRE(undo.undo());
    CHECK(equalIgnoringIdAllocator(model, before));
}

TEST_CASE("MoveClips moves a group sideways and keeps a dissolve inside it")
{
    Fixture f;
    Model before = f.model;
    MoveClips move({f.c, f.d}, 100, 0); // c+d (dissolved) right by 100
    REQUIRE(move.apply(f.model));
    CHECK(f.model.clip(f.c).position == before.clip(f.c).position + 100);
    CHECK(f.model.clip(f.d).position == before.clip(f.d).position + 100);
    CHECK(f.model.hasTransition(f.cd));
    CHECK(f.model.hasTransition(f.ab));
    checkRoundTrip(f.model, move, before);
}

TEST_CASE("MoveClips removes a dissolve whose partner stays behind, and undo brings it back")
{
    Fixture f;
    Model before = f.model;
    MoveClips move({f.b, f.c}, 50, 0); // b leaves a behind (their dissolve goes); c leaves d behind too
    // c moving right by 50 would overlap d, which stays: refused.
    CHECK_FALSE(move.apply(f.model));
    CHECK(equalIgnoringIdAllocator(f.model, before));

    MoveClips alone({f.b}, 50, 0); // b alone into the gap after it
    REQUIRE(alone.apply(f.model));
    CHECK_FALSE(f.model.hasTransition(f.ab));
    checkRoundTrip(f.model, alone, before);
}

TEST_CASE("MoveClips moves across tracks together and refuses what MoveClip would")
{
    Fixture f;
    TrackId v2 = f.model.addTrack(Track::Kind::Video, 1, "V2");
    ClipId e = f.model.insertClip(v2, f.asset, 1000, 0, 49);
    Model before = f.model;

    MoveClips down({f.c, f.d}, 0, 1); // onto V2, dissolve doesn't come along
    REQUIRE(down.apply(f.model));
    CHECK(f.model.clip(f.c).track == v2);
    CHECK(f.model.clip(f.d).track == v2);
    CHECK_FALSE(f.model.hasTransition(f.cd));
    checkRoundTrip(f.model, down, before);

    CHECK_FALSE(MoveClips({f.a}, 0, 5).apply(f.model));    // no such track
    CHECK_FALSE(MoveClips({f.a}, -10, 0).apply(f.model));  // before frame 0
    CHECK_FALSE(MoveClips({e}, -1000, -1).apply(f.model)); // onto a at 0..
    CHECK_FALSE(MoveClips({f.a}, 0, 0).apply(f.model));    // not a move
    CHECK(equalIgnoringIdAllocator(f.model, before));
    f.model.setTrackFlags(v2, false, false, /*locked=*/true);
    CHECK_FALSE(MoveClips({f.c}, 0, 1).apply(f.model));
}
