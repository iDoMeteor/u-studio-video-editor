// core::retime(): a project moved to another frame rate keeps everything
// where it was in time (doc 12, "Frame rate").

#include "doctest.h"

#include "core/model/model.h"
#include "core/model/retime.h"

#include <random>

using namespace ustudio::core;

namespace {

Profile at(Rational fps)
{
    Profile profile;
    profile.fps = fps;
    return profile;
}

AssetId longAsset(Model &model, FrameIndex length)
{
    Asset asset;
    asset.path = "/media/long.mp4";
    asset.info.hasVideo = true;
    asset.info.lengthInSequenceFrames = length;
    return model.addAsset(asset);
}

std::vector<std::string> problems(const Project &project)
{
    return Model(project).check();
}

} // namespace

TEST_CASE("retime: 30 to 60 doubles every frame index, 60 to 30 halves it")
{
    Model model = Model::createEmpty(at({30, 1}));
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = longAsset(model, 3000);
    ClipId a = model.insertClip(track, asset, 0, 150, 449);    // 10 s at 0 s
    ClipId b = model.insertClip(track, asset, 300, 900, 1199); // 10 s at 10 s
    model.addMarker(450, "15 s");

    Project doubled = retime(model.project(), {60, 1});
    Model up(doubled);
    CHECK(up.sequence().profile.fps == Rational{60, 1});
    CHECK(up.clip(a).position == 0);
    CHECK(up.clip(a).in == 300);
    CHECK(up.clip(a).length() == 600);
    CHECK(up.clip(b).position == 600);
    CHECK(up.clip(b).in == 1800);
    CHECK(up.sequence().markers[0].at == 900);
    CHECK(up.asset(asset).info.lengthInSequenceFrames == 6000);
    CHECK(problems(doubled).empty());

    Project back = retime(doubled, {30, 1});
    CHECK(Model(back).sequence() == model.sequence());
    // The original is untouched.
    CHECK(model.sequence().profile.fps == Rational{30, 1});
}

TEST_CASE("retime: 24 to 23.976 over ten minutes rounds positions, never lengths")
{
    Model model = Model::createEmpty(at({24, 1}));
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = longAsset(model, 20000);
    std::mt19937 random(7);
    FrameIndex position = 0;
    std::vector<ClipId> clips;
    while (position < 14400) {
        FrameIndex length =
            std::min<FrameIndex>(std::uniform_int_distribution<FrameIndex>(24, 240)(random), 14400 - position);
        clips.push_back(model.insertClip(track, asset, position, position, position + length - 1));
        position += length;
    }

    const Rational ntsc{24000, 1001};
    Project retimed = retime(model.project(), ntsc);
    Model result(retimed);
    CHECK(problems(retimed).empty());
    // Butted clips stay butted, and each cut stays within half a frame of
    // its true time.
    FrameIndex previousEnd = 0;
    for (ClipId id : clips) {
        const Clip &clip = result.clip(id);
        CHECK(clip.position == previousEnd);
        previousEnd = clip.end();
        const double trueSeconds = static_cast<double>(model.clip(id).position) / 24.0;
        const double newSeconds = static_cast<double>(clip.position) * 1001.0 / 24000.0;
        CHECK(std::abs(newSeconds - trueSeconds) <= 0.5 * 1001.0 / 24000.0);
    }
    // 600 s at 23.976 is 14385.6 frames; rounding lengths instead would
    // have kept 14400 (600.6 s, 0.6 s of drift against the audio).
    CHECK(result.sequence().length() == 14386);
}

TEST_CASE("retime: dissolves keep length = extendA + extendB, and fades and keyframes scale")
{
    Model model = Model::createEmpty(at({25, 1}));
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = longAsset(model, 5000);
    ClipId a = model.insertClip(track, asset, 0, 100, 199);
    ClipId b = model.insertClip(track, asset, 100, 1000, 1099);
    model.addTransition(track, a, b, 5, 7); // a 12-frame dissolve
    Project project = model.project();
    Clip &clipA = project.sequences[0].clips.at(a);
    clipA.fadeIn = FadeSpec{10};
    Effect effect;
    effect.service = "volume";
    Param gain;
    gain.name = "level";
    gain.value = 1.0;
    gain.keyframes = {{0, 0.0}, {50, 1.0}};
    effect.params.push_back(gain);
    clipA.effects.push_back(effect);
    REQUIRE(problems(project).empty());

    Project retimed = retime(project, {30000, 1001}); // 25 -> 29.97
    REQUIRE(problems(retimed).empty());
    const Sequence &seq = retimed.sequences[0];
    REQUIRE(seq.transitions.size() == 1);
    const Transition &dissolve = seq.transitions[0];
    CHECK(dissolve.length == dissolve.extendA + dissolve.extendB);
    CHECK(dissolve.length == seq.clips.at(a).end() - seq.clips.at(b).position);
    // 12 frames at 25 = 0.48 s = 14.4 at 29.97; both ends round on their
    // own, so it can come out a frame either way.
    CHECK(std::abs(static_cast<double>(dissolve.length) - 14.386) <= 1.0);
    CHECK(seq.clips.at(a).fadeIn->length == 12);                       // 10 -> 11.99
    CHECK(seq.clips.at(a).effects[0].params[0].keyframes[1].at == 60); // 50 -> 59.94
}

TEST_CASE("retime: the same rate, or a bad one, changes nothing")
{
    Model model = Model::createEmpty(at({30, 1}));
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    model.insertClip(track, longAsset(model, 100), 7, 0, 9);
    CHECK(retime(model.project(), {30, 1}) == model.project());
    CHECK(retime(model.project(), {60, 2}) == model.project());
    CHECK(retime(model.project(), {0, 1}) == model.project());
}
