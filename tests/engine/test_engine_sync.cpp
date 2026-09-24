#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine/engine_sync.h"
#include "engine/factory_policy.h"
#include "random_commands.h"

#include "core/commands/undo_stack.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>

using namespace ustudio::core;
using namespace ustudio::engine;

namespace {

// Mlt::Tractor::track() returns a new, caller-owned wrapper despite
// mlt++'s header comment (sanitizer report S1) -- own it so the test
// itself is leak-clean under ASan.
std::unique_ptr<Mlt::Producer> trackAt(Mlt::Tractor &tractor, int index)
{
    return std::unique_ptr<Mlt::Producer>(tractor.track(index));
}

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

// FactoryPolicy's own contract (factory_policy.h) is "exactly one instance
// per process": Mlt::Factory::init()/close() are process-wide, and this
// binary runs several TEST_CASEs sequentially. A fresh FactoryPolicy per
// TEST_CASE would init()/close() the same global MLT state repeatedly in
// one process -- unverified territory (CLAUDE.md's "reproduce before
// relying" rule) that main.cpp's actual usage (exactly one, for the whole
// process lifetime) never exercises. Share one instance for the whole
// binary instead, matching that real usage.
FactoryPolicy &sharedFactoryPolicy()
{
    static FactoryPolicy policy;
    return policy;
}

} // namespace

TEST_CASE("EngineSync: empty model produces a tractor with just the black backing track")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty();

    EngineSync sync(model);
    CHECK(sync.tractor().count() == 1);
    CHECK(sync.verify().empty());
}

TEST_CASE("EngineSync: one clip on one video track lands in the playlist at the right position")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addGeneratorAsset(model, "color:red");
    model.insertClip(track, asset, 10, 0, 49); // 50 frames at position 10

    EngineSync sync(model);
    CHECK(sync.verify().empty());

    // Index 0 = black; index 1 = the one video track (no audio tracks).
    REQUIRE(sync.tractor().count() == 2);
    std::unique_ptr<Mlt::Producer> raw = trackAt(sync.tractor(), 1);
    REQUIRE(raw != nullptr);
    Mlt::Playlist playlist(*raw);
    CHECK(playlist.get_length() == 60); // 10 blank + 50 clip
}

TEST_CASE("EngineSync: a clip with video/audio disabled plays from a producer with video_index/audio_index=-1")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty();
    TrackId video = model.addTrack(Track::Kind::Video, 0, "V1");
    TrackId audio = model.addTrack(Track::Kind::Audio, 1, "A1");
    AssetId asset = addGeneratorAsset(model, "color:red");

    // Simulates SplitAudio's two resulting clips: the original with its
    // audio silenced, and the extracted one with its video silenced.
    ClipId videoOnly = model.insertClip(video, asset, 0, 0, 49);
    model.setClipEnabled(videoOnly, /*videoEnabled=*/true, /*audioEnabled=*/false);
    ClipId audioOnly = model.insertClip(audio, asset, 0, 0, 49);
    model.setClipEnabled(audioOnly, /*videoEnabled=*/false, /*audioEnabled=*/true);

    EngineSync sync(model);
    CHECK(sync.verify().empty()); // verify() doesn't check these properties, but the graph shape must still hold

    // On the cut's parent, not the cut: MLT ignores both properties on a
    // cut (see EngineSync::masterProducerFor()), so each clip must play
    // from its own variant of the asset's master producer.
    Mlt::Playlist videoPlaylist(*trackAt(sync.tractor(), 2)); // index 0=black, 1=audio (lowest), 2=video
    std::unique_ptr<Mlt::Producer> videoCut(videoPlaylist.get_clip(0));
    REQUIRE(videoCut != nullptr);
    CHECK(videoCut->parent().get_int("audio_index") == -1);
    CHECK(videoCut->parent().get_int("video_index") != -1);

    Mlt::Playlist audioPlaylist(*trackAt(sync.tractor(), 1));
    std::unique_ptr<Mlt::Producer> audioCut(audioPlaylist.get_clip(0));
    REQUIRE(audioCut != nullptr);
    CHECK(audioCut->parent().get_int("video_index") == -1);
    CHECK(audioCut->parent().get_int("audio_index") != -1);

    // Two different producers, so one clip's switch never leaks into the
    // other's.
    CHECK(videoCut->get_parent() != audioCut->get_parent());
}

TEST_CASE("EngineSync: a clip whose asset file can't be opened plays as black instead of crashing")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    // Keeps the constructor's own first rebuildAll() clean, so the signal
    // connection below (which can only happen after EngineSync exists)
    // catches the actual missing-asset rebuild, not a rebuild that
    // already happened before anything could subscribe to it.
    addGeneratorAsset(model, "color:blue");

    EngineSync sync(model);
    int unavailableCount = 0;
    std::string unavailablePath;
    sync.mediaUnavailable.connect([&](const std::string &path) {
        ++unavailableCount;
        unavailablePath = path;
    });

    Asset missing;
    missing.path = "/nonexistent/path/does-not-exist.mp4";
    missing.displayName = "missing.mp4";
    missing.info.hasVideo = true;
    missing.info.lengthInSequenceFrames = 50;
    AssetId missingId = model.addAsset(missing);
    model.insertClip(track, missingId, 0, 0, 49); // triggers EngineSync's own auto-resync

    CHECK(unavailableCount == 1);
    CHECK(unavailablePath == missing.path);
    CHECK(sync.verify().empty()); // the substituted resource is the intended fallback, not a sync bug

    // The actual crash this guards against (standalone repro, 2026-09-23):
    // an invalid Mlt::Producer still lets .cut() "succeed" and plants
    // into a playlist/tractor with no error, only segfaulting once a real
    // frame is pulled through the live consumer -- so the real assertion
    // here is simply that this doesn't crash.
    sync.tractor().seek(10);
    std::unique_ptr<Mlt::Frame> frame(sync.tractor().get_frame());
    REQUIRE(frame != nullptr);
    CHECK(frame->is_valid());

    // A second clip on the same (already-cached-as-unavailable) asset
    // must not re-fire the signal or re-attempt the open.
    model.insertClip(track, missingId, 100, 0, 49);
    CHECK(unavailableCount == 1);
}

TEST_CASE("EngineSync: a dissolve transition actually cross-fades, and the pair's total span is unchanged")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId assetA = addGeneratorAsset(model, "color:red");
    AssetId assetB = addGeneratorAsset(model, "color:blue");
    ClipId a = model.insertClip(track, assetA, 0, 0, 9);   // [0, 10), source [0, 9]
    ClipId b = model.insertClip(track, assetB, 10, 3, 12); // [10, 20), source [3, 12] -- 3 frames of head handle
    model.addTransition(track, a, b, 3, 3);                // length 6

    EngineSync sync(model);
    CHECK(sync.verify().empty());

    // The pair's combined span is unchanged (Model layer already proves
    // this in isolation -- see test_model.cpp): both clips grew using
    // their own handles, so the track's total length is still exactly
    // A's original 10 + B's original 10, not shorter.
    Mlt::Playlist playlist(*trackAt(sync.tractor(), 1)); // 0 = black, 1 = video
    CHECK(playlist.get_length() == 20);

    // Sample the actual composited frame through the tractor (so the
    // black-track composite is included, matching real playback) at three
    // points: solidly in A's exclusive span, solidly in B's, and
    // mid-dissolve -- confirms the sub-tractor built in
    // buildTransitionSubTractor() plays back correctly end-to-end through
    // EngineSync, not just in the isolated repro it was first verified
    // against (scratchpad/dissolve_repro.cpp).
    int width = sync.profile().width(), height = sync.profile().height();
    auto sampleRedBlue = [&](int frameIndex, int &outR, int &outB) {
        sync.tractor().seek(frameIndex); // get_frame() auto-advances; seek first
        std::unique_ptr<Mlt::Frame> frame(sync.tractor().get_frame());
        mlt_image_format format = mlt_image_rgb;
        uint8_t *image = frame->get_image(format, width, height);
        outR = image[0];
        outB = image[2];
    };

    int r = 0, bch = 0;
    sampleRedBlue(0, r, bch);
    CHECK(r > 200);
    CHECK(bch < 50);

    sampleRedBlue(19, r, bch);
    CHECK(bch > 200);
    CHECK(r < 50);

    int firstOverlapR, firstOverlapB, lastOverlapR, lastOverlapB;
    sampleRedBlue(7, firstOverlapR, firstOverlapB);   // overlap region is [7, 13)
    sampleRedBlue(12, lastOverlapR, lastOverlapB);
    CHECK(firstOverlapR > lastOverlapR);
    CHECK(firstOverlapB < lastOverlapB);
}

TEST_CASE("EngineSync: a clip entirely consumed by its own outgoing transition gets no exclusive segment")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId assetA = addGeneratorAsset(model, "color:red");
    AssetId assetB = addGeneratorAsset(model, "color:blue");
    ClipId a = model.insertClip(track, assetA, 0, 0, 9);    // [0, 10), source [0, 9]
    ClipId b = model.insertClip(track, assetB, 10, 10, 19); // [10, 20), source [10, 19]
    // extendB == a's whole length: b pulls its head all the way back to
    // where a starts, leaving a with no exclusive (pre-overlap) span at
    // all -- planTrackSegments must skip a's Clip segment entirely rather
    // than emit a zero/negative-length one.
    model.addTransition(track, a, b, 0, 10);

    EngineSync sync(model);
    CHECK(sync.verify().empty());

    Mlt::Playlist playlist(*trackAt(sync.tractor(), 1));
    CHECK(playlist.get_length() == 20); // unchanged, same invariant as the test above
    CHECK(playlist.count() == 2);       // the transition sub-tractor, then b's exclusive tail -- no leading blank/clip
}

TEST_CASE("EngineSync: a still image's master producer grows again after a second extension past a cached length "
          "(audit E3)")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");

    // color: is a generator, but EngineSync only looks at asset.info -- not
    // the actual mlt_service -- to decide whether an asset is a still image,
    // so setting isStillImage here exercises the same code path a real
    // pixbuf/qimage asset would. Both default to MLT's usual 15000-frame
    // producer length (verified: a plain "color:" producer's get_length()
    // is 15000 too), so lengths must cross that to actually exercise the
    // bump -- the owner's cited case is an hour-long livestream crossing it
    // within the first eight minutes.
    Asset stillAsset;
    stillAsset.path = "color:red";
    stillAsset.displayName = "watermark.png";
    stillAsset.info.hasVideo = true;
    stillAsset.info.isStillImage = true;
    stillAsset.info.lengthInSequenceFrames = 20'000;
    AssetId assetId = model.addAsset(stillAsset);
    ClipId clip = model.insertClip(track, assetId, 0, 0, 19'999); // 20,000 frames

    EngineSync sync(model);
    REQUIRE(sync.verify().empty());
    {
        Mlt::Playlist playlist(*trackAt(sync.tractor(), 1));
        CHECK(playlist.get_length() == 20'000);
    }

    // Simulates a later ResizeClip (InsertClip::apply/ResizeClip::apply both
    // now call Model::extendAssetLength) stretching the same still further,
    // well past the length its ALREADY-CACHED master producer was bumped to
    // on the first rebuildAll() above. Before the fix, masterProducerFor()
    // only checked/bumped a still's producer length on cache miss, so this
    // second rebuild would silently keep cutting at the old 20,000-frame
    // length instead of the clip's new, longer span.
    model.resizeClip(clip, 0, 39'999, 0); // 40,000 frames
    model.extendAssetLength(assetId, 40'000);

    Mlt::Playlist playlist(*trackAt(sync.tractor(), 1));
    CHECK(playlist.get_length() == 40'000);
}

namespace {
double peakAmplitude(Mlt::Producer &producer)
{
    std::unique_ptr<Mlt::Frame> frame(producer.get_frame());
    mlt_audio_format format = mlt_audio_s16;
    int frequency = 48000;
    int channels = 2;
    int samples = 1920;
    auto *audio = static_cast<int16_t *>(frame->get_audio(format, frequency, channels, samples));
    double peak = 0.0;
    for (int i = 0; i < samples * channels; ++i)
        peak = std::max(peak, std::abs(static_cast<double>(audio[i])));
    return peak;
}
} // namespace

TEST_CASE("EngineSync: Track::volume attaches a volume filter that actually scales the track's audio")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty();
    TrackId fullVolume = model.addTrack(Track::Kind::Audio, 0, "A1");
    TrackId quietTrack = model.addTrack(Track::Kind::Audio, 1, "A2");
    model.setTrackVolume(quietTrack, 0.1); // -20dB

    AssetId tone = model.addAsset([] {
        Asset asset;
        asset.path = "tone:880"; // explicit argument -- "tone:" alone hits verify()'s
                                 // shorthand-stripping edge case for an empty argument
        asset.displayName = "tone:880";
        asset.info.hasAudio = true;
        asset.info.lengthInSequenceFrames = 100;
        return asset;
    }());
    model.insertClip(fullVolume, tone, 0, 0, 99);
    model.insertClip(quietTrack, tone, 0, 0, 99);

    EngineSync sync(model);
    // Not sync.verify(): its resource check reports a pre-existing,
    // unrelated mismatch for audio-only generator producers specifically
    // ("tone:" cuts report the "<producer>" placeholder in a way verify()'s
    // shorthand-stripping doesn't handle -- every other EngineSync test
    // uses a video generator instead, which doesn't hit this). Track
    // count/order is enough to know the right tracks are being measured.
    REQUIRE(sync.tractor().count() == 3);

    // Index 0 = black, 1 = fullVolume (audio tracks come first, model
    // order), 2 = quietTrack.
    double loud = peakAmplitude(*trackAt(sync.tractor(), 1));
    double quiet = peakAmplitude(*trackAt(sync.tractor(), 2));
    REQUIRE(loud > 0.0);
    CHECK(quiet / loud == doctest::Approx(0.1).epsilon(0.02));
}

TEST_CASE("EngineSync: rebuildAll after a model change keeps verify() clean")
{
    sharedFactoryPolicy();
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
    sharedFactoryPolicy();
    Model model = Model::createEmpty();
    // Model order (visual, top to bottom): topVideo, bottomVideo, then audio.
    TrackId topVideo = model.addTrack(Track::Kind::Video, 0, "V-top");
    TrackId bottomVideo = model.addTrack(Track::Kind::Video, 1, "V-bottom");
    TrackId audio = model.addTrack(Track::Kind::Audio, 2, "A1");

    // One clip per track, each on a distinguishable generator resource, so
    // the physical MLT track index each one lands at can actually be read
    // back and checked -- verify() alone can't catch an ordering bug: it
    // cross-checks the tractor against m_mltTrackOrder, which was populated
    // by the same mltTrackOrder() call that built the tractor, so it's
    // self-consistent with whatever order was produced, right or wrong.
    model.insertClip(topVideo, addGeneratorAsset(model, "color:top"), 0, 0, 9);
    model.insertClip(bottomVideo, addGeneratorAsset(model, "color:bottom"), 0, 0, 9);
    model.insertClip(audio, addGeneratorAsset(model, "color:audio"), 0, 0, 9);

    EngineSync sync(model);
    // Index 0 = black, 1 = audio, 2 = bottomVideo, 3 = topVideo (doc 03:
    // audio gets the lowest indices; video is bottom-to-top, i.e. reversed
    // from the model's top-to-bottom visual order).
    REQUIRE(sync.tractor().count() == 4);
    CHECK(sync.verify().empty());

    auto resourceAt = [&](int index) -> std::string {
        std::unique_ptr<Mlt::Producer> raw = trackAt(sync.tractor(), index);
        REQUIRE(raw != nullptr);
        Mlt::Playlist playlist(*raw);
        std::unique_ptr<Mlt::ClipInfo> info(playlist.clip_info(0));
        REQUIRE(info != nullptr);
        return info->resource ? info->resource : "";
    };
    CHECK(resourceAt(1) == "audio");
    CHECK(resourceAt(2) == "bottom");
    CHECK(resourceAt(3) == "top");

    // Re-run rebuildAll and confirm verify() still holds with all three
    // model tracks present, exercising the mixed audio+video ordering path.
    sync.rebuildAll();
    CHECK(sync.verify().empty());
}

TEST_CASE("EngineSync: automatically resyncs when the model changes, with no explicit rebuildAll() call")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addGeneratorAsset(model, "color:red");

    EngineSync sync(model);
    CHECK(sync.tractor().get_length() == 1); // empty sequence: length() is 0, clamped to 1

    // No sync.rebuildAll() here: EngineSync subscribes to Model::changed
    // itself (connectToModel()) and resyncs on its own.
    model.insertClip(track, asset, 0, 0, 99);
    CHECK(sync.tractor().get_length() == 100);
    CHECK(sync.verify().empty());

    model.removeClip(model.track(track).clips.front());
    CHECK(sync.tractor().get_length() == 1);
    CHECK(sync.verify().empty());
}

TEST_CASE("EngineSync: Model::splitClip is one rebuild, not two (audit C5)")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addGeneratorAsset(model, "color:red");
    ClipId clip = model.insertClip(track, asset, 0, 0, 99);

    EngineSync sync(model);
    int rebuiltCount = 0;
    sync.rebuilt.connect([&] { ++rebuiltCount; });

    // splitClip() internally does an insertClip() (its own ClipInserted)
    // plus a handful of raw field copies onto the new right-hand clip
    // followed by a ClipResized -- without batching, each of those two
    // notifies drives its own immediate rebuildAll().
    model.splitClip(clip, 40);
    CHECK(rebuiltCount == 1);
    CHECK(sync.verify().empty());
}

TEST_CASE("EngineSync: reset() called repeatedly never accumulates duplicate Model::changed subscriptions "
          "(audit C4)")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addGeneratorAsset(model, "color:red");

    EngineSync sync(model);
    int rebuiltCount = 0;
    sync.rebuilt.connect([&] { ++rebuiltCount; });

    // "Open Project" calls reset() once per load; simulate several loads in
    // a row on the same EngineSync/Model pair. Before the fix, each
    // reset() added a second, independent subscription to Model::changed
    // on top of the one from construction (and every previous reset()),
    // so a single model edit afterwards would fire onModelEvent() -- and
    // therefore rebuildAll() -- once per accumulated subscription instead
    // of once.
    for (int i = 0; i < 5; ++i)
        sync.reset();
    rebuiltCount = 0; // only care about what happens AFTER the resets

    model.insertClip(track, asset, 0, 0, 9);
    CHECK(rebuiltCount == 1);
    CHECK(sync.verify().empty());
}

TEST_CASE("EngineSync: tractor length matches sequence length, including after growth and shrink")
{
    sharedFactoryPolicy();
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
TEST_CASE("EngineSync property: verify() never fails across 50 random edits")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addGeneratorAsset(model, "color:yellow", 1'000'000);

    EngineSync sync(model);

    std::mt19937 rng(7);
    std::vector<ClipId> liveClips;
    FrameIndex nextFreePosition = 0;

    for (int i = 0; i < 50; ++i) {
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

// doc 12, M1: "EngineSync::verify() never fails across the property test".
// The first 500 commands of tests/core's undo property stream (shared
// generator, same seed), with a live EngineSync resyncing on every model
// change: verify() after every executed command, then after every undo
// while unwinding the whole run back to the start. Every step rebuilds the
// whole tractor, so the cost grows with the square of the length: 500 is
// the owner's chosen size (2026-09-24); the core test alone does 10k.
TEST_CASE("EngineSync property: verify() never fails across 500 undoable commands")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addGeneratorAsset(model, "color:yellow", 1'000'000);
    UndoStack undoStack(model);
    undoStack.limit = 1'000; // unwind all of it, as the core test does
    EngineSync sync(model);

    int failures = 0;
    auto check = [&](const char *phase, int step) {
        std::vector<std::string> problems = sync.verify();
        if (!problems.empty() && ++failures <= 3)
            MESSAGE(phase << " " << step << ": " << problems.front());
    };
    int applied = ustudio::testing::runRandomCommands(model, undoStack, track, asset, 2026, 500,
                                                      [&](int i) { check("after command", i); });
    REQUIRE(applied > 0);
    for (int i = 0; i < applied; ++i) {
        REQUIRE(undoStack.undo());
        check("after undo", i);
    }
    CHECK(failures == 0);
    CHECK(model.sequence().clips.empty());
}

TEST_CASE("EngineSync: a hidden track shows what's under it and a muted track is silent")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty();
    TrackId top = model.addTrack(Track::Kind::Video, 0, "V2");
    TrackId bottom = model.addTrack(Track::Kind::Video, 1, "V1");
    TrackId audio = model.addTrack(Track::Kind::Audio, 2, "A1");
    model.insertClip(top, addGeneratorAsset(model, "color:red"), 0, 0, 29);
    model.insertClip(bottom, addGeneratorAsset(model, "color:blue"), 0, 0, 29);
    Asset toneAsset;
    toneAsset.path = "tone:";
    toneAsset.info.hasAudio = true;
    toneAsset.info.lengthInSequenceFrames = 100'000;
    ClipId tone = model.insertClip(audio, model.addAsset(toneAsset), 0, 0, 29);
    model.setClipEnabled(tone, false, true);
    EngineSync sync(model);

    auto sample = [&](int &red, int &blue, double &rms) {
        sync.tractor().seek(5);
        std::unique_ptr<Mlt::Frame> frame(sync.tractor().get_frame());
        mlt_image_format format = mlt_image_rgb;
        int w = 64, h = 36;
        const uint8_t *image = frame->get_image(format, w, h);
        red = image[0];
        blue = image[2];
        mlt_audio_format audioFormat = mlt_audio_s16;
        int frequency = 48000, channels = 2, samples = 1600;
        const auto *pcm = static_cast<const int16_t *>(frame->get_audio(audioFormat, frequency, channels, samples));
        double sum = 0;
        for (int i = 0; pcm && i < samples * channels; ++i)
            sum += static_cast<double>(pcm[i]) * pcm[i];
        rms = std::sqrt(sum / (samples * channels));
    };

    int red = 0, blue = 0;
    double rms = 0;
    sample(red, blue, rms);
    CHECK(red > 200);
    CHECK(rms > 1000.0);

    const Track &topTrack = model.track(top);
    model.setTrackFlags(top, topTrack.muted, /*hidden=*/true, topTrack.locked);
    const Track &audioTrack = model.track(audio);
    model.setTrackFlags(audio, /*muted=*/true, audioTrack.hidden, audioTrack.locked);
    sample(red, blue, rms);
    CHECK(red < 50);
    CHECK(blue > 200);
    CHECK(rms < 1.0);
    CHECK(sync.verify().empty());
}

TEST_CASE("EngineSync: preview scale builds the playback tractor on a scaled profile")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty(); // 1920x1080 @ 30
    AssetId asset = addGeneratorAsset(model, "color:red");
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    model.insertClip(track, asset, 10, 0, 49);

    EngineSync sync(model, PreviewScale::Half);
    int rebuilds = 0;
    sync.rebuilt.connect([&] { ++rebuilds; });
    CHECK(sync.profile().width() == 960);
    CHECK(sync.profile().height() == 540);
    CHECK(sync.profile().frame_rate_num() == 30);
    CHECK(sync.profile().frame_rate_den() == 1);
    CHECK(sync.tractor().get_length() == 60); // positions and lengths don't scale
    CHECK(sync.verify().empty());

    sync.setPreviewScale(PreviewScale::Full);
    CHECK(rebuilds == 1);
    CHECK(sync.profile().width() == 1920);
    CHECK(sync.profile().height() == 1080);
    CHECK(sync.verify().empty());

    sync.setPreviewScale(PreviewScale::Full); // same factor: no rebuild, no playback restart
    CHECK(rebuilds == 1);
    sync.setPreviewScale(PreviewScale::Auto); // 1080 lines resolves to Full: still no rebuild
    CHECK(rebuilds == 1);

    sync.setPreviewScale(PreviewScale::Quarter);
    CHECK(rebuilds == 2);
    CHECK(sync.profile().width() == 480);
    CHECK(sync.profile().height() == 270);
    CHECK(sync.verify().empty());
}

TEST_CASE("EngineSync: Auto preview scale halves only sequences taller than 1080 lines")
{
    sharedFactoryPolicy();
    Profile uhd;
    uhd.width = 3840;
    uhd.height = 2160;
    uhd.fps = {60, 1};
    Model model4k = Model::createEmpty(uhd);
    EngineSync sync4k(model4k, PreviewScale::Auto);
    CHECK(sync4k.previewFactor() == 0.5);
    CHECK(sync4k.profile().width() == 1920);
    CHECK(sync4k.profile().height() == 1080);

    Model model1080 = Model::createEmpty();
    EngineSync sync1080(model1080, PreviewScale::Auto);
    CHECK(sync1080.previewFactor() == 1.0);
    CHECK(sync1080.profile().height() == 1080);
}

TEST_CASE("EngineSync: scaled preview dimensions are rounded to even numbers")
{
    sharedFactoryPolicy();
    Profile odd;
    odd.width = 1366;
    odd.height = 770;
    Model model = Model::createEmpty(odd);
    EngineSync sync(model, PreviewScale::Half);
    CHECK(sync.profile().width() == 684);  // 683 rounded up to even
    CHECK(sync.profile().height() == 386); // 385 rounded up to even
}
