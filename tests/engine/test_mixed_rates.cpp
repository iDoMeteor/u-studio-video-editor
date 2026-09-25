// Clips of different frame rates on one timeline (doc 12, "Frame rate";
// doc 13 R7): 23.976 to 60 fps sources in a 30 fps and a 24 fps project.
// MLT maps a source into the sequence's rate by time, so each clip should
// last its own duration in project frames, show the source frame at the
// right time on sequential reads (playback), seeks, renders and
// thumbnails, and keep its sound on its cut.
//
// The sources are generated here: each frame a distinct grey (so a decoded
// picture says which source frame it is), a beep on the first three frames
// (so the audio says where the clip starts), encoded losslessly with PCM
// audio so neither shifts.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/concurrency/thread_pool.h"
#include "core/model/model.h"
#include "core/xml/reader.h"
#include "core/xml/writer.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"
#include "engine/thumbnail_cache.h"

#include <framework/mlt_audio.h>
#include <glib.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace ustudio::core;
using namespace ustudio::engine;
namespace fs = std::filesystem;

namespace {

FactoryPolicy &sharedFactoryPolicy()
{
    static FactoryPolicy policy;
    return policy;
}

ustudio::core::concurrency::ThreadPool &testPool()
{
    static ustudio::core::concurrency::ThreadPool pool(4);
    return pool;
}

constexpr int kWidth = 640, kHeight = 360;
const Rational kRates[] = {{24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {50, 1}, {60000, 1001}, {60, 1}};

double rateOf(Rational r)
{
    return static_cast<double>(r.num) / r.den;
}

// About a second of frames.
int framesFor(Rational r)
{
    return static_cast<int>(std::ceil(rateOf(r)));
}

// Steps of 4 (about 3.4 luma levels): the graph can land a picture a
// level darker than the source read alone (a conversion round trip), which
// steps of 3 couldn't tell from the neighbouring frame.
int levelFor(int frame)
{
    return 12 + 4 * frame; // at most 12 + 4 * 59 = 248
}

std::unique_ptr<Mlt::Profile> profileAt(Rational fps)
{
    auto profile = std::make_unique<Mlt::Profile>();
    profile->set_width(kWidth);
    profile->set_height(kHeight);
    profile->set_frame_rate(fps.num, fps.den);
    profile->set_sample_aspect(1, 1);
    profile->set_display_aspect(16, 9);
    profile->set_progressive(1);
    profile->set_colorspace(709);
    return profile;
}

fs::path scratchDir()
{
    static const fs::path dir = [] {
        fs::path d = fs::temp_directory_path() / ("ustudio-mixed-rates-" + std::to_string(::getpid()));
        fs::create_directories(d);
        return d;
    }();
    return dir;
}

struct Cleanup
{
    ~Cleanup()
    {
        std::error_code ec;
        if (!std::getenv("USTUDIO_KEEP_TEST_MEDIA"))
            fs::remove_all(scratchDir(), ec);
    }
} cleanup;

// A lossless source at `fps`: frame i is grey levelFor(i), with a beep on
// frames 0-2.
fs::path makeSource(Rational fps)
{
    const fs::path path = scratchDir() / ("src-" + std::to_string(fps.num) + "-" + std::to_string(fps.den) + ".mov");
    if (fs::exists(path))
        return path;
    auto profile = profileAt(fps);
    const int frames = framesFor(fps);
    Mlt::Playlist video(*profile);
    for (int i = 0; i < frames; ++i) {
        char colour[32];
        std::snprintf(colour, sizeof colour, "color:#%02x%02x%02x", levelFor(i), levelFor(i), levelFor(i));
        Mlt::Producer grey(*profile, colour);
        video.append(grey, 0, 0);
    }
    Mlt::Playlist audio(*profile);
    Mlt::Producer tone(*profile, "tone:");
    tone.set("frequency", 1000);
    audio.append(tone, 0, 2);
    audio.blank(frames - 4);
    Mlt::Tractor tractor(*profile);
    tractor.set_track(video, 0);
    tractor.set_track(audio, 1);
    Mlt::Transition mix(*profile, "mix");
    mix.set("always_active", 1);
    mix.set("sum", 1);
    tractor.plant_transition(mix, 0, 1);

    Mlt::Consumer consumer(*profile, "avformat", path.c_str());
    consumer.set("f", "mov");
    consumer.set("vcodec", "libx264");
    consumer.set("crf", 0); // lossless: a grey level decodes back exactly
    consumer.set("preset", "ultrafast");
    consumer.set("pix_fmt", "yuv420p");
    consumer.set("acodec", "pcm_s16le"); // no encoder delay to shift the beep
    consumer.set("ar", 48000);
    consumer.set("channels", 2);
    consumer.set("real_time", -1);
    consumer.set("terminate_on_pause", 1);
    consumer.connect(tractor);
    consumer.start();
    while (!consumer.is_stopped())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    consumer.stop();
    return path;
}

// Mean luma of the centre of a frame.
double lumaOf(Mlt::Frame &frame)
{
    mlt_image_format format = mlt_image_yuv420p;
    int width = kWidth, height = kHeight;
    const uint8_t *image = frame.get_image(format, width, height);
    double sum = 0;
    int count = 0;
    for (int y = height / 2 - 16; y < height / 2 + 16; ++y)
        for (int x = width / 2 - 16; x < width / 2 + 16; ++x, ++count)
            sum += image[y * width + x];
    return sum / count;
}

double loudnessOf(Mlt::Frame &frame, Rational fps, int position)
{
    mlt_audio_format format = mlt_audio_float;
    int frequency = 48000, channels = 2;
    int samples = mlt_audio_calculate_frame_samples(static_cast<float>(rateOf(fps)), frequency, position);
    const auto *audio = static_cast<const float *>(frame.get_audio(format, frequency, channels, samples));
    double peak = 0;
    for (int i = 0; audio && i < samples * channels; ++i)
        peak = std::max(peak, static_cast<double>(std::abs(audio[i])));
    return peak;
}

// Which source frame a luma is: the nearest of that source's own levels,
// measured by reading the source at its own rate.
struct Source
{
    Rational fps;
    fs::path path;
    std::vector<double> lumas;

    int frameForLuma(double luma) const
    {
        int best = 0;
        for (int i = 1; i < static_cast<int>(lumas.size()); ++i)
            if (std::abs(lumas[i] - luma) < std::abs(lumas[best] - luma))
                best = i;
        return best;
    }
};

const std::vector<Source> &sources()
{
    static const std::vector<Source> all = [] {
        std::vector<Source> list;
        for (Rational fps : kRates) {
            Source source{fps, makeSource(fps), {}};
            auto profile = profileAt(fps);
            Mlt::Producer producer(*profile, source.path.c_str());
            for (int i = 0; i < framesFor(fps); ++i) {
                std::unique_ptr<Mlt::Frame> frame(producer.get_frame());
                source.lumas.push_back(lumaOf(*frame));
            }
            list.push_back(std::move(source));
        }
        return list;
    }();
    return all;
}

struct Placed
{
    const Source *source;
    FrameIndex position = 0;
    FrameIndex length = 0;
};

Model projectAt(Rational fps, std::vector<Placed> &placed)
{
    Profile profile;
    profile.width = kWidth;
    profile.height = kHeight;
    profile.fps = fps;
    Model model = Model::createEmpty(profile);
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    FrameIndex position = 0;
    for (const Source &source : sources()) {
        EngineSync::ProbedMedia probed = EngineSync::probeMediaFile(profile, source.path.string());
        Asset asset;
        asset.path = source.path.string();
        asset.displayName = source.path.filename().string();
        asset.info.hasVideo = true;
        asset.info.hasAudio = probed.hasAudio;
        asset.info.fps = probed.fps;
        asset.info.width = probed.width;
        asset.info.height = probed.height;
        asset.info.lengthInSequenceFrames = probed.length;
        AssetId id = model.addAsset(asset);
        model.insertClip(track, id, position, 0, probed.length - 1);
        placed.push_back({&source, position, probed.length});
        position += probed.length;
    }
    return model;
}

// Checks one frame of the timeline against the source frame due at that
// time.
void checkPicture(const Placed &clip, Rational projectFps, FrameIndex position, double luma)
{
    const double seconds = static_cast<double>(position - clip.position) / rateOf(projectFps);
    const double due = seconds * rateOf(clip.source->fps);
    // The source frame nearest in time (producer_avformat.c rounds:
    // req_position = position / fps * source_fps + 0.5). Matched by
    // "consistent with", not by nearest grey: the colour conversions leave
    // neighbouring greys as little as two levels apart, and the graph can
    // land a level darker than the source read alone.
    bool consistent = false;
    for (int k = static_cast<int>(std::floor(due)) - 1; k <= static_cast<int>(std::ceil(due)) + 1; ++k)
        if (k >= 0 && k < static_cast<int>(clip.source->lumas.size()) && std::abs(k - due) <= 0.5 + 1e-9 &&
            std::abs(clip.source->lumas[static_cast<size_t>(k)] - luma) <= 1.5)
            consistent = true;
    INFO("source " << clip.source->fps.num << "/" << clip.source->fps.den << ", project frame " << position
                   << ": due source frame " << due << ", luma " << luma << " (nearest source frame "
                   << clip.source->frameForLuma(luma) << ")");
    CHECK(consistent);
}

const Placed &clipAt(const std::vector<Placed> &placed, FrameIndex position)
{
    for (const Placed &clip : placed)
        if (position < clip.position + clip.length)
            return clip;
    return placed.back();
}

} // namespace

TEST_CASE("mixed rates: each clip lasts its own duration in project frames")
{
    sharedFactoryPolicy();
    for (Rational projectFps : {Rational{30, 1}, Rational{24, 1}}) {
        std::vector<Placed> placed;
        Model model = projectAt(projectFps, placed);
        for (const Placed &clip : placed) {
            const double seconds = framesFor(clip.source->fps) / rateOf(clip.source->fps);
            INFO("source " << clip.source->fps.num << "/" << clip.source->fps.den << " in a " << projectFps.num
                           << " fps project");
            CHECK(std::abs(static_cast<double>(clip.length) - seconds * rateOf(projectFps)) <= 1.0);
            const Asset &asset = model.asset(model.clip(model.sequence().tracks[0].clips[0]).asset);
            (void)asset;
        }
        // The probe reports each source's own rate, not a rounded one.
        for (size_t i = 0; i < placed.size(); ++i) {
            Rational probed = model.project().bin[i].info.fps;
            CHECK(static_cast<long long>(probed.num) * placed[i].source->fps.den ==
                  static_cast<long long>(placed[i].source->fps.num) * probed.den);
        }
    }
}

TEST_CASE("mixed rates: playback, seeks and cuts show the right picture and sound")
{
    sharedFactoryPolicy();
    for (Rational projectFps : {Rational{30, 1}, Rational{24, 1}}) {
        std::vector<Placed> placed;
        Model model = projectAt(projectFps, placed);
        EngineSync sync(model);
        Mlt::Producer &tractor = sync.tractor();
        const FrameIndex total = placed.back().position + placed.back().length;

        // Sequential, as playback reads it.
        tractor.seek(0);
        std::vector<double> loudness;
        for (FrameIndex position = 0; position < total; ++position) {
            std::unique_ptr<Mlt::Frame> frame(tractor.get_frame());
            checkPicture(clipAt(placed, position), projectFps, position, lumaOf(*frame));
            loudness.push_back(loudnessOf(*frame, projectFps, static_cast<int>(position)));
        }
        // Every clip's beep starts on its own cut, and the frame before is quiet.
        for (const Placed &clip : placed) {
            INFO("cut at " << clip.position << " (source " << clip.source->fps.num << "/" << clip.source->fps.den
                           << ", " << projectFps.num << " fps project)");
            CHECK(loudness[clip.position] > 0.1);
            if (clip.position > 0)
                CHECK(loudness[clip.position - 1] < 0.01);
        }

        // Seeks into the middle of each clip, out of order.
        for (auto it = placed.rbegin(); it != placed.rend(); ++it) {
            const FrameIndex middle = it->position + it->length / 2;
            tractor.seek(static_cast<int>(middle));
            std::unique_ptr<Mlt::Frame> frame(tractor.get_frame());
            checkPicture(*it, projectFps, middle, lumaOf(*frame));
        }
    }
}

TEST_CASE("mixed rates: a render keeps every clip's picture and sound")
{
    sharedFactoryPolicy();
    for (Rational projectFps : {Rational{30, 1}, Rational{24, 1}}) {
        std::vector<Placed> placed;
        Model model = projectAt(projectFps, placed);
        const FrameIndex total = placed.back().position + placed.back().length;
        const fs::path output = scratchDir() / ("render-" + std::to_string(projectFps.num) + ".mov");
        std::string error;
        REQUIRE(renderProject(model, output.string(), error));

        auto profile = profileAt(projectFps);
        Mlt::Producer rendered(*profile, output.c_str());
        REQUIRE(rendered.is_valid());
        std::vector<double> loudness;
        for (FrameIndex position = 0; position < total; ++position) {
            std::unique_ptr<Mlt::Frame> frame(rendered.get_frame());
            checkPicture(clipAt(placed, position), projectFps, position, lumaOf(*frame));
            loudness.push_back(loudnessOf(*frame, projectFps, static_cast<int>(position)));
        }
        // AAC in the render delays nothing at frame granularity here: the
        // beep still starts on each cut (one frame of slack for the codec).
        for (const Placed &clip : placed) {
            INFO("rendered cut at " << clip.position << " (" << projectFps.num << " fps project)");
            CHECK(std::max(loudness[clip.position], loudness[std::min(clip.position + 1, total - 1)]) > 0.1);
            if (clip.position > 1)
                CHECK(loudness[clip.position - 2] < 0.01);
        }
    }
}

TEST_CASE("mixed rates: thumbnails show the frame due at that point")
{
    sharedFactoryPolicy();
    const Rational projectFps{30, 1};
    std::vector<Placed> placed;
    Model model = projectAt(projectFps, placed);
    ThumbnailCache cache(testPool(), [] {}, 2, ustudio::core::concurrency::Priority::Interactive);
    for (const Placed &clip : placed) {
        const FrameIndex offset = clip.length / 2;
        const ThumbnailCache::Data *data = nullptr;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!data && std::chrono::steady_clock::now() < deadline) {
            while (g_main_context_iteration(nullptr, FALSE)) {
            }
            data = cache.frameThumbnail(clip.source->path.string(), static_cast<int>(offset), projectFps.num,
                                        projectFps.den);
            if (!data)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(data != nullptr);
        // Grey: the red channel is the level.
        const size_t centre = (static_cast<size_t>(data->height / 2) * data->width + data->width / 2) * 4;
        const int level = data->rgba[centre];
        const double due = static_cast<double>(offset) / rateOf(projectFps) * rateOf(clip.source->fps);
        int shown = 0;
        for (int i = 1; i < framesFor(clip.source->fps); ++i)
            if (std::abs(levelFor(i) - level) < std::abs(levelFor(shown) - level))
                shown = i;
        INFO("thumbnail of source " << clip.source->fps.num << "/" << clip.source->fps.den << ": due " << due
                                    << ", shown " << shown << " (level " << level << ")");
        CHECK(std::abs(shown - due) <= 1.0);
    }
    cache.shutdown();
}

TEST_CASE("mixed rates: a project saves and loads with every clip and rate intact")
{
    sharedFactoryPolicy();
    std::vector<Placed> placed;
    Model model = projectAt({24, 1}, placed);
    const fs::path file = scratchDir() / "mixed.ustudio";
    REQUIRE(saveProject(model, file.string()).empty());
    auto loaded = loadProject(file.string());
    REQUIRE(loaded.has_value());
    CHECK(loaded->sequence().profile.fps == Rational{24, 1});
    REQUIRE(loaded->sequence().clips.size() == placed.size());
    for (size_t i = 0; i < model.project().bin.size(); ++i) {
        const Asset &before = model.project().bin[i];
        const Asset &after = loaded->project().bin[i];
        CHECK(after.info.fps == before.info.fps);
        CHECK(after.info.lengthInSequenceFrames == before.info.lengthInSequenceFrames);
    }
    for (const auto &[id, clip] : model.sequence().clips) {
        REQUIRE(loaded->hasClip(id));
        CHECK(loaded->clip(id).position == clip.position);
        CHECK(loaded->clip(id).in == clip.in);
        CHECK(loaded->clip(id).out == clip.out);
    }
}
