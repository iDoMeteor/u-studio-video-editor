#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/model/model.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

using namespace ustudio::core;
using namespace ustudio::engine;

namespace {

FactoryPolicy &sharedFactoryPolicy()
{
    static FactoryPolicy policy;
    return policy;
}

// A minimal, valid 2x2 RGBA PNG (half-transparent red), written as bytes
// rather than committed as a binary file (doc 11: no binary media in the
// repo) -- generated once with Python's zlib/struct and pasted in as data.
constexpr unsigned char kTinyPng[] = {137, 80,  78,  71,  13, 10, 26,  10, 0,   0,   0,  13,  73,
                                      72,  68,  82,  0,   0,  0,  2,   0,  0,   0,   2,  8,   6,
                                      0,   0,   0,   114, 182, 13, 36, 0,  0,   0,   17, 73,  68,
                                      65,  84,  120, 156, 99,  248, 207, 192, 208, 0,   194, 12,
                                      48,  6,   0,   56,  232, 5,  253, 17, 51,  48,  201, 0,   0,
                                      0,   0,   73,  69,  78,  68, 174, 66, 96,  130};

struct RemoveOnExit
{
    std::filesystem::path path;
    ~RemoveOnExit()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

} // namespace

TEST_CASE("EngineSync::probeMedia recognizes a still image via its MLT service, not the extension")
{
    sharedFactoryPolicy();
    std::random_device rd;
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("ustudio-probe-media-test-" + std::to_string(rd()) + ".png");
    RemoveOnExit cleanup{path};
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(kTinyPng), sizeof(kTinyPng));
    }

    Model model = Model::createEmpty();
    EngineSync sync(model);

    EngineSync::ProbedMedia probed = sync.probeMedia(path.string());
    CHECK(probed.length > 0);
    CHECK(probed.isStillImage);
    CHECK_FALSE(probed.hasAudio);
}

TEST_CASE("EngineSync::probeMedia reports a generator producer as not a still image")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty();
    EngineSync sync(model);

    // color: is a video generator, not an image file -- the still-image
    // path (pixbuf/qimage) must not misfire on it.
    EngineSync::ProbedMedia probed = sync.probeMedia("color:red");
    CHECK(probed.length > 0);
    CHECK_FALSE(probed.isStillImage);
}

TEST_CASE("EngineSync::probeMedia leaves fps/width/height at 0 for a generator (no meta.media.*)")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty();
    EngineSync sync(model);

    EngineSync::ProbedMedia probed = sync.probeMedia("color:red");
    CHECK(probed.fps.num == 0);
    CHECK(probed.width == 0);
    CHECK(probed.height == 0);
}

TEST_CASE("EngineSync::probeMedia reads real fps/width/height from an actual media file")
{
    sharedFactoryPolicy();
    std::random_device rd;
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("ustudio-probe-media-fps-test-" + std::to_string(rd()) + ".mp4");
    RemoveOnExit cleanup{path};

    // Renders a tiny real MP4 (doc 11: no binary media committed to the
    // repo, generated on the fly instead) via the same avformat/libx264
    // pair renderProject() itself uses as this project's fixed working
    // format (engine_sync.cpp), so meta.media.* is populated the same way
    // a real imported clip's would be.
    {
        Model renderModel = Model::createEmpty();
        EngineSync renderSync(renderModel);
        Mlt::Producer producer(renderSync.profile(), "color:red");
        producer.set_in_and_out(0, 4);
        std::unique_ptr<Mlt::Profile> consumerProfile(producer.profile());
        Mlt::Consumer consumer(*consumerProfile, "avformat", path.string().c_str());
        consumer.set("vcodec", h264Encoder().c_str());
        consumer.connect(producer);
        consumer.run();
        consumer.stop(); // joins the render-ahead thread (notes/render.md)
    }

    Model model = Model::createEmpty();
    EngineSync sync(model);
    EngineSync::ProbedMedia probed = sync.probeMedia(path.string());
    CHECK(probed.fps.num > 0);
    CHECK(probed.fps.den > 0);
    CHECK(probed.width == model.sequence().profile.width);
    CHECK(probed.height == model.sequence().profile.height);
}

TEST_CASE("EngineSync::probeMedia reports hasAudio=false for a real video with no audio stream (2026-09-22 "
         "audit E3)")
{
    sharedFactoryPolicy();
    std::random_device rd;
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("ustudio-probe-media-no-audio-test-" + std::to_string(rd()) + ".mp4");
    RemoveOnExit cleanup{path};

    // Previously guessed as "not a still image" -- wrong for exactly
    // this case (a real, non-still video with a video-only container).
    {
        Model renderModel = Model::createEmpty();
        EngineSync renderSync(renderModel);
        Mlt::Producer producer(renderSync.profile(), "color:red");
        producer.set_in_and_out(0, 4);
        std::unique_ptr<Mlt::Profile> consumerProfile(producer.profile());
        Mlt::Consumer consumer(*consumerProfile, "avformat", path.string().c_str());
        consumer.set("vcodec", h264Encoder().c_str());
        consumer.set("an", 1); // no audio track in the muxed output
        consumer.connect(producer);
        consumer.run();
        consumer.stop(); // joins the render-ahead thread (notes/render.md)
    }

    Model model = Model::createEmpty();
    EngineSync sync(model);
    EngineSync::ProbedMedia probed = sync.probeMedia(path.string());
    REQUIRE(probed.length > 0);
    CHECK_FALSE(probed.isStillImage);
    CHECK_FALSE(probed.hasAudio);
}

TEST_CASE("EngineSync::probeMedia reports hasAudio=true for a real video with an audio stream (2026-09-22 "
         "audit E3)")
{
    sharedFactoryPolicy();
    std::random_device rd;
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("ustudio-probe-media-with-audio-test-" + std::to_string(rd()) + ".mp4");
    RemoveOnExit cleanup{path};

    {
        Model renderModel = Model::createEmpty();
        EngineSync renderSync(renderModel);
        Mlt::Profile &profile = renderSync.profile();

        Mlt::Producer videoProducer(profile, "color:red");
        videoProducer.set_in_and_out(0, 4);
        Mlt::Producer audioProducer(profile, "tone:440");
        audioProducer.set_in_and_out(0, 4);

        Mlt::Tractor tractor(profile);
        tractor.set_track(videoProducer, 0);
        tractor.set_track(audioProducer, 1);
        tractor.set_in_and_out(0, 4);

        std::unique_ptr<Mlt::Profile> consumerProfile(tractor.profile());
        Mlt::Consumer consumer(*consumerProfile, "avformat", path.string().c_str());
        consumer.set("vcodec", h264Encoder().c_str());
        consumer.set("acodec", "aac");
        consumer.connect(tractor);
        consumer.run();
        consumer.stop(); // joins the render-ahead thread (notes/render.md)
    }

    Model model = Model::createEmpty();
    EngineSync sync(model);
    EngineSync::ProbedMedia probed = sync.probeMedia(path.string());
    REQUIRE(probed.length > 0);
    CHECK_FALSE(probed.isStillImage);
    CHECK(probed.hasAudio);
}

TEST_CASE("EngineSync::probeMedia reports hasAudio=false for a generator producer (2026-09-22 audit E3)")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty();
    EngineSync sync(model);

    // color: has no audio_index property at all (not an avformat
    // producer) -- get_int() on a missing property returns 0, which
    // must not be misread as "audio stream 0".
    EngineSync::ProbedMedia probed = sync.probeMedia("color:red");
    CHECK_FALSE(probed.hasAudio);
}

TEST_CASE("EngineSync::probeMedia reports 0 length for a path nothing can open")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty();
    EngineSync sync(model);

    EngineSync::ProbedMedia probed = sync.probeMedia("/nonexistent/path/does-not-exist.mp4");
    CHECK(probed.length == 0);
}
