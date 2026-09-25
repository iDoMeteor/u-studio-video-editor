// engine::decodeEnvelope + core::audio::align on real decoded audio: a
// rendered file of tone bursts at irregular gaps, two spans of it placed
// out of sync on purpose (doc: README "Sync tracks (audio)").

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/audio/align.h"
#include "core/model/model.h"
#include "engine/audio_sync.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"

#include <unistd.h>

#include <cmath>
#include <filesystem>
#include <string>

using namespace ustudio::core;
using namespace ustudio::engine;

namespace {

FactoryPolicy &sharedFactoryPolicy()
{
    static FactoryPolicy policy;
    return policy;
}

// 20 s at 30 fps: tone bursts of varying length at irregular gaps, over a
// black picture. Generated, never committed (CLAUDE.md: no binary media).
std::filesystem::path renderBursts()
{
    std::filesystem::path out =
        std::filesystem::temp_directory_path() / ("ustudio-audio-sync-" + std::to_string(getpid()) + ".mp4");
    Model model = Model::createEmpty();
    TrackId video = model.addTrack(Track::Kind::Video, 0, "V1");
    TrackId audio = model.addTrack(Track::Kind::Audio, 1, "A1");
    Asset black;
    black.path = "color:black";
    black.info.hasVideo = true;
    black.info.lengthInSequenceFrames = 100'000;
    model.insertClip(video, model.addAsset(black), 0, 0, 599);
    Asset tone;
    tone.path = "tone:660";
    tone.info.hasAudio = true;
    tone.info.lengthInSequenceFrames = 100'000;
    AssetId toneId = model.addAsset(tone);
    const int bursts[][2] = {{5, 9}, {20, 4}, {31, 15}, {70, 6}, {83, 3}, {110, 22}, {150, 5}, {162, 11},
                             {210, 7}, {240, 18}, {290, 4}, {301, 9}, {350, 13}, {400, 3}, {420, 26},
                             {470, 8}, {500, 5}, {530, 17}, {570, 6}};
    for (const auto &[at, len] : bursts) {
        ClipId c = model.insertClip(audio, toneId, at, 0, len - 1);
        model.setClipEnabled(c, false, true);
    }
    std::string error;
    REQUIRE(renderProject(model, out.string(), error));
    return out;
}

} // namespace

TEST_CASE("audio sync: two spans of the same recording, 7 frames out, are lined up")
{
    sharedFactoryPolicy();
    std::filesystem::path media = renderBursts();
    const Rational fps{30, 1};

    // A: source frames 30..449 at timeline 30 (where they belong).
    // B: source frames 90..389 at timeline 97, i.e. 7 frames late.
    auto a = decodeEnvelope({media.string(), 30, 420, 30}, fps);
    auto b = decodeEnvelope({media.string(), 90, 300, 97}, fps);
    REQUIRE(a);
    REQUIRE(b);
    auto result = audio::align(*a, *b, 5'000.0, 2'000.0);
    REQUIRE(result);
    MESSAGE("shift " << result->shiftMs << " ms, correlation " << result->correlation << ", runner-up "
                     << result->runnerUp);
    CHECK(std::abs(result->shiftMs - (-7 * 1000.0 / 30.0)) <= 5.0);
    CHECK(result->confident);
    std::filesystem::remove(media);
}

TEST_CASE("audio sync: a file without sound gives no envelope")
{
    sharedFactoryPolicy();
    CHECK_FALSE(decodeEnvelope({"color:red", 0, 30, 0}, Rational{30, 1}));
}
