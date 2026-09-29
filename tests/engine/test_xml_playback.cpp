#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/commands/primitives.h"
#include "core/commands/timeline_edits.h"
#include "core/commands/undo_stack.h"
#include "core/model/transition_native.h"
#include "core/xml/reader.h"
#include "core/xml/writer.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"

#include <mlt++/Mlt.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

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
    consumer.stop(); // joins the render-ahead thread (notes/render.md)
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

// IP2 (format 5): an effect saved with the project plays outside the editor
// too, as a native <filter> MLT's xml producer attaches to the cut, with its
// keyframes as an MLT animation string. brightness's "level" is animated
// (filter_brightness.yml: animation: yes, 0 black ... 1 unchanged), so the
// red clip goes from black to full red across its 50 frames.
TEST_CASE("A saved effect plays via MLT's own xml producer, animated")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset asset;
    asset.path = "color:red";
    asset.info.hasVideo = true;
    asset.info.lengthInSequenceFrames = 100;
    // From source frame 30, not 0: a cut's filter animation counts from the
    // filter's "in" (engine::attachToCut()), which the writer sets to the cut's.
    ClipId clip = model.insertClip(track, model.addAsset(asset), 0, 30, 79);
    Effect fade;
    fade.service = "brightness";
    Param level;
    level.name = "level";
    level.value = 1.0;
    level.keyframes = {{0, 0.0, Easing::Linear}, {49, 1.0, Easing::Linear}};
    fade.params = {level};
    model.addEffect(Model::EffectTarget::clip(clip), fade, 0);

    std::filesystem::path path = tempProjectPath("ustudio-xml-playback-effect");
    RemoveOnExit cleanup{path};
    REQUIRE(saveProject(model, path.string()).empty());

    Mlt::Profile profile("atsc_1080p_30");
    Mlt::Producer loaded(profile, ("xml:" + path.string()).c_str());
    REQUIRE(loaded.is_valid());
    auto redAt = [&](int position) { return sampleFrame(loaded, position, 1920, 1080).r; };
    CHECK(redAt(0) < 20);
    CHECK(std::abs(static_cast<int>(redAt(25)) - 128) < 30);
    CHECK(redAt(49) > 235);
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

TEST_CASE("A dissolve into a generator from its first frame plays the same outside the editor")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty();
    TrackId video = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId red = addGenerator(model, "color:red", true, false);
    Asset boundless; // no known length, as a generator or still: boundless
    boundless.path = "color:blue";
    boundless.displayName = "color:blue";
    boundless.info.hasVideo = true;
    AssetId blue = model.addAsset(boundless);
    ClipId a = model.insertClip(video, red, 0, 0, 29);
    ClipId b = model.insertClip(video, blue, 30, 0, 39); // in 0: no head room as placed
    // Model::addTransition() would give b in -5 (VE Effects, 2026-09-28);
    // the command slips b's window forward instead.
    REQUIRE(AddTransition(video, a, b, 5, 5).apply(model));
    CHECK(model.clip(b).in == 0);
    REQUIRE(model.check().empty());
    EngineSync sync(model);
    CHECK(sync.verify().empty());

    std::filesystem::path path = tempProjectPath("ustudio-xml-boundless-dissolve");
    RemoveOnExit cleanup{path};
    REQUIRE(saveProject(model, path.string()).empty());
    Mlt::Producer loaded(sync.profile(), ("xml:" + path.string()).c_str());
    REQUIRE(loaded.is_valid());
    CHECK(loaded.get_length() == sync.tractor().get_length());
    const int width = sync.profile().width(), height = sync.profile().height();
    for (int position : {20, 25, 30, 34, 40, 69}) {
        INFO("frame " << position);
        FrameSample live = sampleFrame(sync.tractor(), position, width, height);
        FrameSample saved = sampleFrame(loaded, position, width, height);
        CHECK(std::abs(live.r - saved.r) <= 2);
        CHECK(std::abs(live.b - saved.b) <= 2);
    }
    FrameSample mid = sampleFrame(sync.tractor(), 30, width, height);
    CHECK(mid.r > 40);
    CHECK(mid.b > 40);
}

// FX3 (doc 15): a transition recipe (a wipe's map, a dip's brightness ramps)
// comes from core::nativeTransition() in both the editor's graph and the
// saved file, so melt plays what the editor shows. Sampled across the
// width, since a wipe's two halves differ where a dissolve's don't.
TEST_CASE("A saved wipe and dip play frame-identically outside the editor")
{
    sharedFactoryPolicy();

    Model model = Model::createEmpty();
    TrackId video = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId red = addGenerator(model, "color:red", true, false);
    AssetId blue = addGenerator(model, "color:blue", true, false);
    AssetId green = addGenerator(model, "color:green", true, false);
    ClipId a = model.insertClip(video, red, 0, 0, 29);    // [0, 30)
    ClipId b = model.insertClip(video, blue, 30, 10, 49); // [30, 70)
    ClipId c = model.insertClip(video, green, 70, 10, 49); // [70, 110)
    TransitionId wipe = model.addTransition(video, a, b, 5, 5); // [25, 35)
    TransitionId dip = model.addTransition(video, b, c, 5, 5);  // [65, 75)
    model.setTransitionRecipe(wipe, "wipe.left",
                              {{"video.service", std::string("luma"), {}},
                               {"video.luma", std::string("left"), {}},
                               {"video.softness", 0.1, {}}});
    model.setTransitionRecipe(dip, "dip-black",
                              {{"a.0.service", std::string("brightness"), {}},
                               {"a.0.level", std::string("ramp:1,0,0"), {}},
                               {"b.0.service", std::string("brightness"), {}},
                               {"b.0.level", std::string("ramp:0,0,1"), {}}});
    REQUIRE(model.check().empty());
    EngineSync sync(model);
    CHECK(sync.verify().empty());

    // A folder of its own: the writer puts the wipe's map beside the project.
    std::filesystem::path dir = tempProjectPath("ustudio-xml-wipe").replace_extension();
    std::filesystem::create_directories(dir);
    struct RemoveDir
    {
        std::filesystem::path path;
        ~RemoveDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{dir};
    std::filesystem::path path = dir / "wipe.ustudio";
    REQUIRE(saveProject(model, path.string()).empty());
    CHECK(std::filesystem::is_regular_file(lumaMapPath(dir / "ustudio-wipes", "left")));

    Mlt::Producer loaded(sync.profile(), ("xml:" + path.string()).c_str());
    REQUIRE(loaded.is_valid());
    CHECK(loaded.get_length() == sync.tractor().get_length());

    const int width = sync.profile().width(), height = sync.profile().height();
    auto row = [&](Mlt::Producer &producer, int position) {
        producer.seek(position);
        std::unique_ptr<Mlt::Frame> frame(producer.get_frame());
        mlt_image_format format = mlt_image_rgb;
        int w = width, h = height;
        const uint8_t *image = frame->get_image(format, w, h);
        std::vector<std::array<int, 3>> out;
        for (int x : {w / 10, w / 2, w * 9 / 10}) {
            const uint8_t *p = image + (static_cast<size_t>(h / 2) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3;
            out.push_back({p[0], p[1], p[2]});
        }
        return out;
    };
    int mismatches = 0;
    for (int position = 20; position < 80; ++position) {
        auto live = row(sync.tractor(), position), saved = row(loaded, position);
        for (size_t i = 0; i < live.size(); ++i)
            for (size_t k = 0; k < 3; ++k)
                if (std::abs(live[i][k] - saved[i][k]) > 2 && ++mismatches <= 5)
                    MESSAGE("frame " << position << " sample " << i << " channel " << k << ": live " << live[i][k]
                                     << " vs saved " << saved[i][k]);
    }
    CHECK(mismatches == 0);

    // What the comparison relies on: mid-wipe the left is already blue and
    // the right still red; mid-dip the picture is dark.
    auto midWipe = row(sync.tractor(), 31);
    CHECK(midWipe[0][2] > 200);
    CHECK(midWipe[0][0] < 50);
    CHECK(midWipe[2][0] > 200);
    CHECK(midWipe[2][2] < 50);
    auto midDip = row(sync.tractor(), 70);
    for (const auto &sample : midDip)
        CHECK(sample[0] + sample[1] + sample[2] < 60);
}

// FX3 motion: a push is the affine transition sliding the next clip in and
// an affine filter on the outgoing cut sliding it out; the saved file must
// move both the same way (the filter's in/out are the cut's, as
// attachToCut() sets them).
TEST_CASE("A saved push plays frame-identically outside the editor")
{
    sharedFactoryPolicy();

    Model model = Model::createEmpty();
    TrackId video = model.addTrack(Track::Kind::Video, 0, "V1");
    ClipId a = model.insertClip(video, addGenerator(model, "color:red", true, false), 0, 0, 29); // [0, 30)
    ClipId b = model.insertClip(video, addGenerator(model, "color:blue", true, false), 30, 10, 49);
    TransitionId push = model.addTransition(video, a, b, 10, 10); // [20, 40)
    model.setTransitionRecipe(push, "push.left",
                              {{"video.service", std::string("affine"), {}},
                               {"video.rect", std::string("ramp:100% 0% 100% 100%|0% 0% 100% 100%"), {}},
                               {"a.0.service", std::string("affine"), {}},
                               {"a.0.transition.rect", std::string("ramp:0% 0% 100% 100%|-100% 0% 100% 100%"), {}}});
    REQUIRE(model.check().empty());
    EngineSync sync(model);
    CHECK(sync.verify().empty());

    std::filesystem::path path = tempProjectPath("ustudio-xml-push");
    RemoveOnExit cleanup{path};
    REQUIRE(saveProject(model, path.string()).empty());
    Mlt::Producer loaded(sync.profile(), ("xml:" + path.string()).c_str());
    REQUIRE(loaded.is_valid());

    const int width = sync.profile().width(), height = sync.profile().height();
    // The first x from the left that is blue on the middle row; -1 if none.
    auto edge = [&](Mlt::Producer &producer, int position) {
        producer.seek(position);
        std::unique_ptr<Mlt::Frame> frame(producer.get_frame());
        mlt_image_format format = mlt_image_rgb;
        int w = width, h = height;
        const uint8_t *image = frame->get_image(format, w, h);
        for (int x = 0; x < w; x += 2) {
            const uint8_t *p = image + (static_cast<size_t>(h / 2) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3;
            if (p[2] > 150 && p[0] < 100)
                return x;
        }
        return -1;
    };
    int previous = width;
    for (int position = 20; position < 40; ++position) {
        const int live = edge(sync.tractor(), position), saved = edge(loaded, position);
        INFO("frame " << position << ": live edge " << live << ", saved " << saved);
        CHECK(std::abs(live - saved) <= 2);
        // The incoming picture comes in from the right, steadily.
        if (position > 20) {
            CHECK(live >= 0);
            CHECK(live <= previous);
        }
        if (live >= 0)
            previous = live;
    }
    CHECK(edge(sync.tractor(), 30) > width / 4);
    CHECK(edge(sync.tractor(), 30) < width * 3 / 4);
}

// M5's gate (doc 12): dissolves and wipes survive a move or trim of either
// clip and undo/redo, checked by the verifier and an XML round trip, and
// melt plays them as the editor does.
TEST_CASE("M5 gate: a wipe and a dip survive moves, trims, undo and redo; verified, round-tripped, played by melt")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty();
    TrackId video = model.addTrack(Track::Kind::Video, 0, "V1");
    ClipId a = model.insertClip(video, addGenerator(model, "color:red", true, false), 0, 0, 49);      // [0, 50)
    ClipId b = model.insertClip(video, addGenerator(model, "color:blue", true, false), 50, 20, 69);   // [50, 100)
    ClipId c = model.insertClip(video, addGenerator(model, "color:green", true, false), 100, 20, 69); // [100, 150)
    TransitionId wipe = model.addTransition(video, a, b, 5, 5);
    TransitionId dip = model.addTransition(video, b, c, 5, 5);
    const std::vector<Param> wipeParams{{"video.service", std::string("luma"), {}},
                                        {"video.luma", std::string("left"), {}},
                                        {"video.softness", 0.1, {}}};
    const std::vector<Param> dipParams{{"a.0.service", std::string("brightness"), {}},
                                       {"a.0.level", std::string("ramp:1,0,0"), {}},
                                       {"b.0.service", std::string("brightness"), {}},
                                       {"b.0.level", std::string("ramp:0,0,1"), {}}};
    model.setTransitionRecipe(wipe, "wipe.left", wipeParams);
    model.setTransitionRecipe(dip, "dip-black", dipParams);
    REQUIRE(model.check().empty());

    std::filesystem::path dir = tempProjectPath("ustudio-m5-gate").replace_extension();
    std::filesystem::create_directories(dir);
    struct RemoveDir
    {
        std::filesystem::path path;
        ~RemoveDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{dir};

    int step = 0;
    auto checkAll = [&](const std::string &what) {
        INFO("after " << what);
        REQUIRE(model.check().empty());
        REQUIRE(model.hasTransition(wipe));
        REQUIRE(model.hasTransition(dip));
        CHECK(model.transition(wipe).params == wipeParams);
        CHECK(model.transition(dip).params == dipParams);
        EngineSync sync(model);
        CHECK(sync.verify().empty());
        const std::filesystem::path path = dir / ("step" + std::to_string(step++) + ".ustudio");
        REQUIRE(saveProject(model, path.string()).empty());
        auto loaded = loadProject(path.string());
        REQUIRE(loaded.has_value());
        CHECK(*loaded == model);
        Mlt::Producer melt(sync.profile(), ("xml:" + path.string()).c_str());
        REQUIRE(melt.is_valid());
        // The middle of each transition, and either side: the same picture.
        const int w = sync.profile().width(), h = sync.profile().height();
        for (TransitionId id : {wipe, dip}) {
            const Transition &t = model.transition(id);
            const FrameIndex start = model.clip(t.b).position;
            for (FrameIndex frame : {start - 3, start + t.length / 2, start + t.length + 2}) {
                const FrameSample live = sampleFrame(sync.tractor(), static_cast<int>(frame), w, h);
                const FrameSample saved = sampleFrame(melt, static_cast<int>(frame), w, h);
                CHECK(std::abs(live.r - saved.r) <= 2);
                CHECK(std::abs(live.g - saved.g) <= 2);
                CHECK(std::abs(live.b - saved.b) <= 2);
            }
        }
    };

    checkAll("setting up");
    UndoStack undo(model);
    // The whole group moved on: both transitions go with it.
    REQUIRE(undo.execute(std::make_unique<MoveClips>(std::vector<ClipId>{a, b, c}, 20, 0)));
    checkAll("moving all three clips");
    // a's head trimmed (far from the wipe on its tail) ...
    const Clip &clipA = model.clip(a);
    REQUIRE(undo.execute(std::make_unique<ResizeClip>(a, clipA.in + 5, clipA.out, clipA.position + 5)));
    checkAll("trimming a's head");
    // ... and c's tail (far from the dip on its head).
    const Clip &clipC = model.clip(c);
    REQUIRE(undo.execute(std::make_unique<ResizeClip>(c, clipC.in, clipC.out - 10, clipC.position)));
    checkAll("trimming c's tail");
    for (int i = 0; i < 3; ++i) {
        REQUIRE(undo.undo());
        checkAll("undo " + std::to_string(i + 1));
    }
    for (int i = 0; i < 3; ++i) {
        REQUIRE(undo.redo());
        checkAll("redo " + std::to_string(i + 1));
    }
}

// FX3 audio curves: a transition's "audio.start" -2 is MLT's equal-power
// crossfade (transition_mix.yml), -1 (the default) the even one. With the
// same tone on both sides, an even crossfade sums to the tone's level at
// the middle and an equal-power one to about 1.41 times it; melt the same.
TEST_CASE("A transition's equal-power crossfade is louder mid-way than the even one, in melt too")
{
    sharedFactoryPolicy();
    auto midRms = [&](std::optional<double> start, double &meltRms) {
        Model model = Model::createEmpty();
        TrackId audio = model.addTrack(Track::Kind::Audio, 0, "A1");
        AssetId tone = addGenerator(model, "tone:", false, true);
        ClipId a = model.insertClip(audio, tone, 0, 0, 59);
        ClipId b = model.insertClip(audio, tone, 60, 20, 79); // 20 frames of head room
        TransitionId t = model.addTransition(audio, a, b, 10, 10); // [50, 70)
        (void)b;
        if (start)
            model.setTransitionRecipe(t, "equal-power", {{"audio.start", *start, {}}});
        EngineSync sync(model);
        const std::filesystem::path path = tempProjectPath("ustudio-audio-curve");
        RemoveOnExit cleanup{path};
        REQUIRE(saveProject(model, path.string()).empty());
        Mlt::Producer melt(sync.profile(), ("xml:" + path.string()).c_str());
        REQUIRE(melt.is_valid());
        // Float samples: an equal-power middle is louder than full scale.
        auto rms = [](Mlt::Producer &producer) {
            producer.seek(60);
            std::unique_ptr<Mlt::Frame> frame(producer.get_frame());
            mlt_audio_format format = mlt_audio_float;
            int frequency = 48000, channels = 2, samples = 1600;
            const auto *pcm = static_cast<const float *>(frame->get_audio(format, frequency, channels, samples));
            double sum = 0.0;
            for (int i = 0; pcm && i < samples * channels; ++i)
                sum += static_cast<double>(pcm[i]) * pcm[i];
            return samples > 0 ? std::sqrt(sum / (samples * channels)) : 0.0;
        };
        meltRms = rms(melt);
        return rms(sync.tractor());
    };
    double meltEven = 0, meltPower = 0;
    const double even = midRms(std::nullopt, meltEven);
    const double power = midRms(-2.0, meltPower);
    INFO("even " << even << ", equal power " << power << "; melt " << meltEven << ", " << meltPower);
    CHECK(even > 0.1);
    CHECK(power / even == doctest::Approx(1.414).epsilon(0.1));
    CHECK(meltEven == doctest::Approx(even).epsilon(0.02));
    CHECK(meltPower == doctest::Approx(power).epsilon(0.02));
}

// FX3 leftover: zoom and spin are the affine transition too, with a rect
// growing from the middle and (spin) an animated fix_rotate_x; melt plays
// them as the editor does.
TEST_CASE("A saved zoom and spin play frame-identically outside the editor")
{
    sharedFactoryPolicy();
    for (bool spin : {false, true}) {
        INFO(std::string(spin ? "spin" : "zoom"));
        Model model = Model::createEmpty();
        TrackId video = model.addTrack(Track::Kind::Video, 0, "V1");
        ClipId a = model.insertClip(video, addGenerator(model, "color:red", true, false), 0, 0, 29);
        ClipId b = model.insertClip(video, addGenerator(model, "color:blue", true, false), 30, 10, 49);
        TransitionId t = model.addTransition(video, a, b, 10, 10); // [20, 40)
        std::vector<Param> params{{"video.service", std::string("affine"), {}},
                                  {"video.rect", std::string("ramp:50% 50% 0% 0%|0% 0% 100% 100%"), {}}};
        if (spin)
            params.push_back({"video.fix_rotate_x", std::string("ramp:-180,0"), {}});
        model.setTransitionRecipe(t, spin ? "spin.in.right" : "zoom.in", params);
        REQUIRE(model.check().empty());
        EngineSync sync(model);
        const std::filesystem::path path = tempProjectPath("ustudio-xml-zoom");
        RemoveOnExit cleanup{path};
        REQUIRE(saveProject(model, path.string()).empty());
        Mlt::Producer loaded(sync.profile(), ("xml:" + path.string()).c_str());
        REQUIRE(loaded.is_valid());
        const int w = sync.profile().width(), h = sync.profile().height();
        for (int frame : {21, 25, 30, 35, 39}) {
            const FrameSample live = sampleFrame(sync.tractor(), frame, w, h);
            const FrameSample saved = sampleFrame(loaded, frame, w, h);
            INFO("frame " << frame << " live " << live.r << "," << live.b << " saved " << saved.r << "," << saved.b);
            CHECK(std::abs(live.r - saved.r) <= 2);
            CHECK(std::abs(live.b - saved.b) <= 2);
        }
        // Early on only the middle is the incoming clip; by the end, all of it.
        CHECK(sampleFrame(sync.tractor(), 25, w, h).b > 150);
        CHECK(sampleFrame(sync.tractor(), 39, w, h).b > 150);
    }
}
