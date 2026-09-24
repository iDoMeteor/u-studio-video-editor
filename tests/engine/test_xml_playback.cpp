#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/xml/writer.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"

#include <mlt++/Mlt.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <random>
#include <string>

using namespace ustudio::core;
using namespace ustudio::engine;

namespace {
// RAII so the temp file is removed even if a REQUIRE below throws --
// doctest's REQUIRE failure unwinds via an exception, which would
// otherwise skip a plain std::remove() at the end of the test body.
struct RemoveOnExit
{
    std::filesystem::path path;
    ~RemoveOnExit()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

// One FactoryPolicy for the whole binary: its contract is exactly one per
// process (see test_engine_sync.cpp's sharedFactoryPolicy()).
FactoryPolicy &sharedFactoryPolicy()
{
    static FactoryPolicy policy;
    return policy;
}

// A random suffix, not a fixed name: two concurrent test runs (agent
// worktrees, or a local run racing CI) could otherwise collide on the same
// path mid-write (CLAUDE.md's concurrent-session hazard).
std::filesystem::path tempProjectPath(const std::string &stem)
{
    std::random_device rd;
    return std::filesystem::temp_directory_path() / (stem + "-" + std::to_string(rd()) + ".ustudio");
}

AssetId addGenerator(Model &model, const std::string &resource, bool video, bool audio)
{
    Asset asset;
    asset.path = resource;
    asset.displayName = resource;
    asset.info.hasVideo = video;
    asset.info.hasAudio = audio;
    asset.info.lengthInSequenceFrames = 100'000;
    return model.addAsset(asset);
}

// A short real MP4 with an audio stream (tone), rendered on the fly (no
// binary media in the repo): the per-clip audio switch (audio_index = -1)
// only means something to avformat producers, so a generator can't test it.
void renderToneClip(Mlt::Profile &profile, const std::filesystem::path &path, int frames)
{
    Mlt::Tractor tractor(profile);
    Mlt::Producer video(profile, "color:gray");
    video.set_in_and_out(0, frames - 1);
    Mlt::Producer audio(profile, "tone:");
    audio.set_in_and_out(0, frames - 1);
    tractor.set_track(video, 0);
    tractor.set_track(audio, 1);
    std::unique_ptr<Mlt::Field> field(tractor.field());
    Mlt::Transition mix(profile, "mix");
    mix.set("start", 1.0);
    mix.set("sum", 1);
    mix.set("always_active", 1);
    field->plant_transition(mix, 0, 1);
    Mlt::Consumer consumer(profile, "avformat", path.string().c_str());
    consumer.set("vcodec", h264Encoder().c_str());
    consumer.set("acodec", "aac");
    consumer.set("real_time", -1);
    consumer.connect(tractor);
    consumer.run();
}

// The composited picture's centre pixel and the frame's audio RMS, pulled
// the way a consumer would (seek, then get_frame: get_frame auto-advances).
struct FrameSample
{
    int r = 0, g = 0, b = 0;
    double rms = 0.0;
};

FrameSample sampleFrame(Mlt::Producer &producer, int position, int width, int height)
{
    producer.seek(position);
    std::unique_ptr<Mlt::Frame> frame(producer.get_frame());
    FrameSample sample;
    mlt_image_format format = mlt_image_rgb;
    int w = width, h = height;
    const uint8_t *image = frame->get_image(format, w, h);
    size_t centre = (static_cast<size_t>(h / 2) * static_cast<size_t>(w) + static_cast<size_t>(w / 2)) * 3;
    sample.r = image[centre];
    sample.g = image[centre + 1];
    sample.b = image[centre + 2];

    mlt_audio_format audioFormat = mlt_audio_s16;
    int frequency = 48000, channels = 2, samples = 1600; // 48 kHz / 30 fps
    const auto *pcm = static_cast<const int16_t *>(frame->get_audio(audioFormat, frequency, channels, samples));
    double sum = 0.0;
    for (int i = 0; pcm && i < samples * channels; ++i)
        sum += static_cast<double>(pcm[i]) * pcm[i];
    sample.rms = samples > 0 ? std::sqrt(sum / (samples * channels)) : 0.0;
    return sample;
}
} // namespace

// doc 12's M1 acceptance: "melt saved.ustudio (or u-studio-render) plays
// the saved file with no editor". `melt` itself isn't installed on this
// machine, so this exercises the same path melt uses internally: MLT's
// own "xml" producer service loading our writer's output, completely
// independent of core/xml's reader (which is OUR parser, not MLT's).
//
// v1's README documents a real, empirically-confirmed MLT quirk here:
// reloading XML gives back a plain producer (Service::type() ==
// mlt_service_producer_type), not a tractor -- wrapping it as
// Mlt::Tractor to keep editing it fails. That's irrelevant to THIS
// criterion, though: melt only needs to PLAY the file, not re-edit it as
// a C++ Tractor object (doc 09: our reader reconstructs the editable
// model from ustudio:* properties directly; the MLT structure is output,
// never read back in as input). Confirmed below: is_valid() and
// get_frame() both work on the wrapped producer regardless of its
// reported service type.
TEST_CASE("A file our writer saves plays via MLT's own xml producer, independent of our reader")
{
    sharedFactoryPolicy();

    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");

    Asset asset;
    asset.path = "color:red";
    asset.info.hasVideo = true;
    asset.info.lengthInSequenceFrames = 100;
    AssetId assetId = model.addAsset(asset);
    model.insertClip(track, assetId, 0, 0, 49);

    std::filesystem::path path = tempProjectPath("ustudio-xml-playback-test");
    RemoveOnExit cleanup{path};
    REQUIRE(saveProject(model, path.string()).empty());

    Mlt::Profile profile;
    Mlt::Producer loaded(profile, ("xml:" + path.string()).c_str());
    REQUIRE(loaded.is_valid());
    CHECK(loaded.get_length() == 50);

    std::unique_ptr<Mlt::Frame> frame(loaded.get_frame());
    REQUIRE(frame != nullptr);
    CHECK(frame->is_valid());
}

// doc 12's M1 box again, for everything the editor's own playback graph
// does beyond a plain clip: a dissolve (the pair overlaps on the track, and
// the clip after it must not shift), per-track volume, and a clip with its
// audio disabled (Split Audio's result). Every frame of the saved file,
// played by MLT's own xml producer (what `melt` does), must match live
// EngineSync playback of the same model: picture and sound.
TEST_CASE("A saved project with a dissolve, track volume and a muted clip plays frame-identically outside the editor")
{
    sharedFactoryPolicy();

    Model model = Model::createEmpty();
    TrackId video = model.addTrack(Track::Kind::Video, 0, "V1");
    TrackId audio = model.addTrack(Track::Kind::Audio, 1, "A1");
    TrackId audio2 = model.addTrack(Track::Kind::Audio, 2, "A2");
    AssetId red = addGenerator(model, "color:red", true, false);
    AssetId blue = addGenerator(model, "color:blue", true, false);
    AssetId green = addGenerator(model, "color:green", true, false);
    AssetId tone = addGenerator(model, "tone:", false, true);

    ClipId a = model.insertClip(video, red, 0, 0, 29);    // [0, 30)
    ClipId b = model.insertClip(video, blue, 30, 10, 49); // [30, 70), 10 frames of head handle
    model.insertClip(video, green, 70, 0, 19);            // [70, 90): shifts if the dissolve is mis-written
    model.addTransition(video, a, b, 5, 5);               // overlap [25, 35)

    // Hidden and muted tracks (MLT's per-track "hide"): if the saved file
    // ignored either flag, its picture would turn white / its level rise.
    TrackId hiddenTop = model.addTrack(Track::Kind::Video, 0, "V0 hidden");
    model.insertClip(hiddenTop, addGenerator(model, "color:white", true, false), 0, 0, 89);
    model.setTrackFlags(hiddenTop, false, /*hidden=*/true, false);
    TrackId mutedAudio = model.addTrack(Track::Kind::Audio, 4, "A3 muted");
    ClipId mutedTone = model.insertClip(mutedAudio, tone, 0, 0, 89);
    model.setClipEnabled(mutedTone, false, true);
    model.setTrackFlags(mutedAudio, /*muted=*/true, false, false);

    ClipId loud = model.insertClip(audio, tone, 0, 0, 44); // [0, 45)
    model.setClipEnabled(loud, false, true);
    model.setTrackVolume(audio, 0.5);
    EngineSync sync(model);

    // A real file on A2 with its audio switched off (what Split Audio leaves
    // on the video side): must stay silent outside the editor too.
    std::filesystem::path media = tempProjectPath("ustudio-xml-tone").replace_extension(".mp4");
    RemoveOnExit mediaCleanup{media};
    renderToneClip(sync.profile(), media, 100);
    Asset toneFile;
    toneFile.path = media.string();
    toneFile.displayName = "tone.mp4";
    toneFile.info.hasVideo = true;
    toneFile.info.hasAudio = true;
    toneFile.info.lengthInSequenceFrames = 100;
    AssetId toneFileId = model.addAsset(toneFile);
    ClipId silent = model.insertClip(audio2, toneFileId, 0, 0, 89);
    model.setClipEnabled(silent, false, false);
    REQUIRE(model.check().empty());
    for (const std::string &problem : sync.verify())
        MESSAGE("verify: " << problem);
    CHECK(sync.verify().empty());

    std::filesystem::path path = tempProjectPath("ustudio-xml-dissolve-test");
    RemoveOnExit cleanup{path};
    REQUIRE(saveProject(model, path.string()).empty());

    Mlt::Producer loaded(sync.profile(), ("xml:" + path.string()).c_str());
    REQUIRE(loaded.is_valid());
    CHECK(loaded.get_length() == sync.tractor().get_length());

    int width = sync.profile().width(), height = sync.profile().height();
    int mismatches = 0;
    for (int position = 0; position < sync.tractor().get_length(); ++position) {
        FrameSample live = sampleFrame(sync.tractor(), position, width, height);
        FrameSample saved = sampleFrame(loaded, position, width, height);
        bool picture =
            std::abs(live.r - saved.r) <= 2 && std::abs(live.g - saved.g) <= 2 && std::abs(live.b - saved.b) <= 2;
        bool sound = std::abs(live.rms - saved.rms) <= std::max(1.0, live.rms * 0.02);
        if (!picture || !sound) {
            if (++mismatches <= 5)
                MESSAGE("frame " << position << ": live rgb(" << live.r << "," << live.g << "," << live.b << ") rms "
                                 << live.rms << " vs saved rgb(" << saved.r << "," << saved.g << "," << saved.b
                                 << ") rms " << saved.rms);
        }
    }
    CHECK(mismatches == 0);

    // And the live graph really does what the comparison relies on: a
    // cross-fade mid-dissolve, the green clip exactly at 70, and the
    // volume/mute actually applied (otherwise matching would prove little).
    FrameSample mid = sampleFrame(sync.tractor(), 30, width, height);
    CHECK(mid.r > 40);
    CHECK(mid.b > 40);
    CHECK(sampleFrame(sync.tractor(), 70, width, height).g > 100);
    CHECK(sampleFrame(sync.tractor(), 10, width, height).rms > 100.0);
    CHECK(sampleFrame(sync.tractor(), 60, width, height).rms < 1.0);
}
