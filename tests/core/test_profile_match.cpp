// The first-video-clip rule (doc 13 R7), frame-rate notes, and
// SetSequenceProfile.

#include "doctest.h"

#include "core/commands/primitives.h"
#include "core/commands/undo_stack.h"
#include "core/model/model.h"
#include "core/model/profile_match.h"
#include "core/model/retime.h"

using namespace ustudio::core;

namespace {

Asset videoAsset(Rational fps, bool still = false)
{
    Asset asset;
    asset.path = "/media/clip.mp4";
    asset.info.hasVideo = true;
    asset.info.width = 1920;
    asset.info.height = 1080;
    asset.info.fps = fps;
    asset.info.isStillImage = still;
    asset.info.lengthInSequenceFrames = 300;
    return asset;
}

} // namespace

TEST_CASE("profile match: fps text")
{
    CHECK(formatFps({24000, 1001}) == "23.976");
    CHECK(formatFps({30000, 1001}) == "29.97");
    CHECK(formatFps({60000, 1001}) == "59.94");
    CHECK(formatFps({25, 1}) == "25");
    CHECK(formatFps({0, 1}) == "—");
}

TEST_CASE("profile match: only an empty project takes the first video's format")
{
    Model model = Model::createEmpty();
    CHECK(sequenceTakesProfileFromMedia(model.project()));

    // A still image or an audio file first doesn't decide it.
    model.addAsset(videoAsset({0, 1}, true));
    Asset audio;
    audio.path = "/media/voice.wav";
    audio.info.hasAudio = true;
    model.addAsset(audio);
    CHECK(sequenceTakesProfileFromMedia(model.project()));

    // A video in the bin does.
    AssetId video = model.addAsset(videoAsset({24000, 1001}));
    CHECK_FALSE(sequenceTakesProfileFromMedia(model.project()));

    // So does a clip on the timeline, whatever the bin holds.
    Model withClip = Model::createEmpty();
    TrackId track = withClip.addTrack(Track::Kind::Video, 0, "V1");
    Asset colour;
    colour.path = "color:red";
    colour.info.hasVideo = true;
    colour.info.lengthInSequenceFrames = 100;
    AssetId colourId = withClip.addAsset(colour);
    withClip.insertClip(track, colourId, 0, 0, 9);
    CHECK_FALSE(sequenceTakesProfileFromMedia(withClip.project()));
    (void)video;
}

TEST_CASE("profile match: the media's profile, and notes for other rates")
{
    Profile profile = profileForMedia(Profile{}, 3840, 2160, {60000, 1001});
    CHECK(profile.width == 3840);
    CHECK(profile.height == 2160);
    CHECK(profile.fps == Rational{60000, 1001});
    CHECK(profile.dar == Rational{16, 9});
    CHECK(profile.mltName.empty());

    CHECK(frameRateNote("a.mp4", {24, 1}, {30, 1}) == "a.mp4 is 24 fps; the project is 30, so frames will repeat");
    CHECK(frameRateNote("b.mp4", {60000, 1001}, {30, 1}) ==
          "b.mp4 is 59.94 fps; the project is 30, so frames will be skipped");
    CHECK(frameRateNote("c.mp4", {60, 2}, {30, 1}).empty()); // the same rate
    CHECK(frameRateNote("d.png", {0, 1}, {30, 1}).empty());
}

TEST_CASE("SetSequenceProfile: sets and undoes on an empty sequence, refuses one with clips")
{
    Model model = Model::createEmpty();
    UndoStack undo(model);
    Profile hd = profileForMedia(model.sequence().profile, 1280, 720, {50, 1});
    REQUIRE(undo.execute(std::make_unique<SetSequenceProfile>(hd)));
    CHECK(model.sequence().profile == hd);
    undo.undo();
    CHECK(model.sequence().profile == Profile{});

    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset colour;
    colour.path = "color:red";
    colour.info.hasVideo = true;
    colour.info.lengthInSequenceFrames = 100;
    model.insertClip(track, model.addAsset(colour), 0, 0, 9);
    CHECK_FALSE(undo.execute(std::make_unique<SetSequenceProfile>(hd)));
    CHECK(model.sequence().profile == Profile{});
}

TEST_CASE("retimeFrame: rounds half up, exactly")
{
    CHECK(retimeFrame(300, {30, 1}, {60, 1}) == 600);
    CHECK(retimeFrame(601, {60, 1}, {30, 1}) == 301);           // 300.5 rounds up
    CHECK(retimeFrame(14400, {24, 1}, {24000, 1001}) == 14386); // 14385.6
    // An hour at 60000/1001 doesn't overflow.
    CHECK(retimeFrame(215784, {60000, 1001}, {30, 1}) == 108000);
}

TEST_CASE("frameRateSummary: one short sentence however many files, grouped by rate")
{
    const Rational p30{30, 1};
    std::vector<std::pair<std::string, Rational>> clips;
    for (int i = 0; i < 8; ++i)
        clips.emplace_back("chunk_00" + std::to_string(i) + ".mp4", Rational{25, 1});
    CHECK(frameRateSummary(clips, p30) == "8 are 25 fps; the project is 30, so frames will repeat");

    clips.emplace_back("match.mp4", p30); // matching clips aren't counted
    clips.emplace_back("fast.mp4", Rational{60, 1});
    clips.emplace_back("ntsc.mp4", Rational{24000, 1001});
    CHECK(frameRateSummary(clips, p30) == "1 is 23.976 fps (frames will repeat), 8 are 25 fps (frames will repeat), "
                                          "1 is 60 fps (frames will be skipped); the project is 30");

    CHECK(frameRateSummary({{"a.mp4", Rational{60, 1}}, {"b.mp4", Rational{50, 1}}}, p30) ==
          "1 is 50 fps, 1 is 60 fps; the project is 30, so frames will be skipped");
    // One differing file: the per-file sentence.
    CHECK(frameRateSummary({{"one.mp4", Rational{25, 1}}, {"ok.mp4", p30}}, p30) ==
          frameRateNote("one.mp4", Rational{25, 1}, p30));
    CHECK(frameRateSummary({{"ok.mp4", p30}, {"still.png", Rational{0, 0}}}, p30).empty());
    CHECK(frameRateSummary({}, p30).empty());
}
