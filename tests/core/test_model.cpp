#include "doctest.h"

#include "core/commands/composite_command.h"
#include "core/commands/primitives.h"
#include "core/commands/undo_stack.h"
#include "core/model/model.h"

#include <algorithm>
#include <chrono>
#include <memory>
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

TEST_CASE("Model: a copy gets its own, empty changed Signal, never the source's subscribers (audit C4)")
{
    Model original = Model::createEmpty();
    int firedOnOriginal = 0;
    original.changed.connect([&](const ModelEvent &) { ++firedOnOriginal; });

    Model copy = original; // copy-construct
    int firedOnCopy = 0;
    // If the copy had inherited original's subscriber list, this would be
    // the SECOND slot in it; instead it must be the copy's only slot.
    copy.changed.connect([&](const ModelEvent &) { ++firedOnCopy; });

    TrackId track = copy.addTrack(Track::Kind::Video, 0, "V1");
    (void)track;
    CHECK(firedOnCopy == 1);
    CHECK(firedOnOriginal == 0); // original's own subscriber never saw the copy's edit
}

TEST_CASE("Model: assignment keeps the target's existing changed subscribers (audit C4)")
{
    Model target = Model::createEmpty();
    int firedOnTarget = 0;
    target.changed.connect([&](const ModelEvent &) { ++firedOnTarget; });

    Model source = Model::createEmpty();
    source.addTrack(Track::Kind::Video, 0, "from-source");

    // Simulates "Open Project": target (the live, subscribed-to model) is
    // reassigned wholesale from a freshly-loaded Model that has never had
    // anything connect to its own `changed`. Before the fix, this silently
    // replaced target's `changed` (subscribers and all) with source's
    // (empty) one -- exactly the class of bug that leaves whatever was
    // listening (EngineSync, in practice) permanently disconnected.
    target = std::move(source);
    CHECK(firedOnTarget == 0);
    target.addTrack(Track::Kind::Video, 0, "after-assignment");
    CHECK(firedOnTarget == 1);
}

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

TEST_CASE("Model: addTransition extends both clips from their own handles; the pair's combined span is unchanged")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model, 300);
    ClipId a = model.insertClip(track, asset, 0, 0, 49);     // [0, 50), source [0, 49]
    ClipId b = model.insertClip(track, asset, 50, 100, 149); // [50, 100), source [100, 149]
    FrameIndex originalBEnd = model.clip(b).end();

    TransitionId t = model.addTransition(track, a, b, 6, 4); // length 10

    CHECK(model.check().empty());
    CHECK(model.transition(t).length == 10);
    // a: position fixed, out extends forward by extendA.
    CHECK(model.clip(a).position == 0);
    CHECK(model.clip(a).out == 55);
    // b: in and position both pull back by extendB, out unchanged -- so
    // b's own end point (and everything that would come after it on this
    // track) never moves.
    CHECK(model.clip(b).position == 46);
    CHECK(model.clip(b).in == 96);
    CHECK(model.clip(b).out == 149);
    CHECK(model.clip(b).end() == originalBEnd);
    // The two clips now overlap by exactly the transition's length.
    CHECK(model.clip(a).end() - model.clip(b).position == 10);
}

TEST_CASE("Model: removeTransition is the exact inverse of addTransition")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model, 300);
    ClipId a = model.insertClip(track, asset, 0, 0, 49);
    ClipId b = model.insertClip(track, asset, 50, 100, 149);
    Clip beforeA = model.clip(a);
    Clip beforeB = model.clip(b);

    TransitionId t = model.addTransition(track, a, b, 6, 4);
    model.removeTransition(t);

    CHECK(model.check().empty());
    CHECK_FALSE(model.hasTransition(t));
    CHECK(model.clip(a) == beforeA);
    CHECK(model.clip(b) == beforeB);
}

TEST_CASE("Model: check() still flags an overlap whose width doesn't match its covering transition")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model, 300);
    ClipId a = model.insertClip(track, asset, 0, 0, 49);
    ClipId b = model.insertClip(track, asset, 50, 100, 149);
    model.addTransition(track, a, b, 6, 4); // length 10

    // Bypass the mutator (Model::mutableSequence() is the documented
    // escape hatch) to widen the overlap without updating the
    // transition's recorded length -- check() must not just trust that a
    // *some* transition links the pair, the width has to match exactly.
    model.mutableSequence().clips.at(b).position -= 5;

    CHECK_FALSE(model.check().empty());
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

TEST_CASE("Model::snapshot: an immutable copy, shared until the next edit (doc 19, MT0)")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");

    std::shared_ptr<const Project> first = model.snapshot();
    REQUIRE(first);
    CHECK(*first == model.project());
    CHECK(model.snapshot() == first); // no edit between: the same copy

    model.setTrackName(track, "Renamed");
    std::shared_ptr<const Project> second = model.snapshot();
    CHECK(second != first); // the edit invalidated the cache
    CHECK(second->sequences[0].tracks[0].name == "Renamed");
    CHECK(first->sequences[0].tracks[0].name == "V1"); // the old one is untouched

    // Wholesale replacement (Open Project) invalidates it too.
    Model other = Model::createEmpty();
    model = other;
    CHECK(model.snapshot() != second);
    CHECK(*model.snapshot() == other.project());
}

namespace {

// Inserts n clips in timeline order through one undoable command (a caption
// import's shape), shifts every clip right as a ripple does, then undoes the
// insert. Returns the milliseconds taken; checks the result on the way.
double bulkEditMs(int n)
{
    Model model = Model::createEmpty();
    const TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset still;
    still.path = "color:red";
    still.info.hasVideo = true;
    still.info.isStillImage = true;
    const AssetId asset = model.addAsset(still);
    UndoStack undo(model);
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::unique_ptr<Command>> inserts;
    for (int i = 0; i < n; ++i)
        inserts.push_back(std::make_unique<InsertClip>(track, asset, FrameIndex{i} * 10, 0, 7));
    REQUIRE(undo.execute(std::make_unique<CompositeCommand>("Insert clips", std::move(inserts))));
    const std::vector<ClipId> ids = model.track(track).clips;
    for (auto it = ids.rbegin(); it != ids.rend(); ++it) // the last first, as a ripple does
        model.moveClip(*it, track, model.clip(*it).position + 2);
    for (ClipId id : ids) // and back
        model.moveClip(id, track, model.clip(id).position - 2);
    CHECK(model.track(track).clips == ids);
    undo.undo();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    CHECK(model.track(track).clips.empty());
    CHECK(model.check().empty());
    return ms;
}

} // namespace

TEST_CASE("Model: bulk inserts, ripples and their undo scale near-linearly with the clip count")
{
    // Every placement used to re-sort the whole track (a hash lookup per
    // comparison), and every range check scanned it: 10x the clips took
    // ~100x as long, so a 20,000-cue caption file took minutes. Placing one
    // clip is now binary searches, so 10x the clips takes ~11-13x here.
    // Judged as a ratio, not a time limit (the machine's speed and load
    // don't matter); best of three for each size.
    auto best = [](int n) {
        double ms = bulkEditMs(n);
        for (int run = 0; run < 2; ++run)
            ms = std::min(ms, bulkEditMs(n));
        return ms;
    };
    const double small = best(1500), large = best(15000);
    INFO("1,500 clips: " << small << " ms; 15,000: " << large << " ms");
    CHECK(large < small * 30); // quadratic: ~100
}
