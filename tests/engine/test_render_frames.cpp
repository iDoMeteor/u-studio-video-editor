// M6 groundwork, M5's gate: `u-studio-render --frames` hashes every frame as
// the editor's preview draws it, and --ffv1 renders the same pixels
// losslessly. Checked against the live Engine's frames (the ones the
// preview widget shows), not against another EngineSync.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/media/frame_hash.h"
#include "core/media/utf8_path.h"
#include "core/model/model.h"
#include "core/xml/writer.h"
#include "engine/engine.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"
#include "platform/process.h"
#include "render/frames_command.h"

#include <glib.h>
#include <mlt++/Mlt.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>

using namespace ustudio;
using namespace ustudio::core;
using namespace ustudio::engine;
namespace fs = std::filesystem;

namespace {

FactoryPolicy &sharedFactoryPolicy()
{
    static FactoryPolicy policy;
    return policy;
}

template <class Done> bool pumpUntil(Done done, std::chrono::milliseconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_iteration(nullptr, FALSE)) {
        }
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// Red under a quarter-size, rotated blue picture on V2 that dissolves into
// green: a transform, a composite and a dissolve, over 60 frames.
Model makeModel()
{
    Model model = Model::createEmpty();
    const TrackId lower = model.addTrack(Track::Kind::Video, 0, "V1");
    const TrackId upper = model.addTrack(Track::Kind::Video, 0, "V2");
    auto colour = [&](const char *path) {
        Asset asset;
        asset.path = path;
        asset.info.hasVideo = true;
        asset.info.lengthInSequenceFrames = 10'000;
        return model.addAsset(asset);
    };
    model.insertClip(lower, colour("color:red"), 0, 0, 59);
    const ClipId blue = model.insertClip(upper, colour("color:blue"), 0, 0, 34);
    const ClipId green = model.insertClip(upper, colour("color:green"), 35, 100, 124);
    Transform placed;
    placed.bounds = Transform::Bounds::None;
    placed.x.value = 480;
    placed.y.value = 270;
    placed.width.value = 960;
    placed.height.value = 540;
    // Keyframed (M5 box 2): x eased across the clip and into the dissolve's
    // tail, rotation on other frames (sampled).
    placed.x.keyframes = {{0, 480, Easing::CubicInOut}, {38, 1200, Easing::Linear}};
    placed.rotation.keyframes = {{6, 0, Easing::Linear}, {30, 25, Easing::Linear}};
    model.setClipTransform(blue, placed);
    REQUIRE(model.check().empty());
    REQUIRE(model.addTransition(upper, blue, green, 5, 5).value != 0);
    return model;
}

std::map<int, std::string> parseHashes(const std::string &json)
{
    std::map<int, std::string> hashes;
    static const std::regex entry(R"re(\{"frame":(\d+),"hash":"([0-9a-f]{16})"\})re");
    for (auto it = std::sregex_iterator(json.begin(), json.end(), entry); it != std::sregex_iterator(); ++it)
        hashes[std::stoi((*it)[1])] = (*it)[2];
    return hashes;
}

} // namespace

TEST_CASE("u-studio-render --frames: every frame hashes as the preview shows it, and --ffv1 keeps those pixels")
{
    sharedFactoryPolicy();
    const fs::path dir = fs::temp_directory_path() / ("ustudio-frames-" + std::to_string(platform::currentProcessId()));
    fs::create_directories(dir);
    Model model = makeModel();
    const std::string project = utf8String(dir / "frames.ustudio");
    REQUIRE(saveProject(model, project).empty());

    const dropins::RenderSubcommand frames = render::framesSubcommand(nullptr);
    std::ostringstream out;
    REQUIRE(frames.run({project}, out) == 0);
    const std::map<int, std::string> rendered = parseHashes(out.str());
    CHECK(rendered.size() == 60); // the whole sequence, 0-59
    CHECK(out.str().find(R"("width":1920,"height":1080,"fps":"30/1")") != std::string::npos);

    // The live preview: the Engine the editor runs, its frames as the widget gets them.
    Engine engine(model.snapshot(), PreviewScale::Full);
    std::mutex mutex;
    std::map<int, std::string> shown;
    engine.setFrameCallback([&](std::vector<uint8_t> rgba, int, int, int position) {
        std::lock_guard<std::mutex> lock(mutex);
        shown[position] = frameHash(rgba.data(), rgba.size());
    });
    for (int position : {0, 12, 30, 31, 36, 50, 59}) {
        INFO("frame " << position);
        engine.seek(position);
        REQUIRE(pumpUntil(
            [&] {
                std::lock_guard<std::mutex> lock(mutex);
                return shown.contains(position);
            },
            std::chrono::seconds(10)));
        std::lock_guard<std::mutex> lock(mutex);
        REQUIRE(rendered.contains(position));
        CHECK(shown.at(position) == rendered.at(position));
    }
    engine.shutdown();

    // A lossless render of part of it decodes to exactly the graph's own YUV
    // (frames_command.h).
    const std::string lossless = utf8String(dir / "frames.mkv");
    std::ostringstream ffv1;
    REQUIRE(frames.run({project, "--range", "25:39", "--ffv1", lossless}, ffv1) == 0);
    CHECK(ffv1.str().find(R"("frames":15)") != std::string::npos);
    CHECK_FALSE(fs::exists(lossless + ".part"));
    Mlt::Profile profile("atsc_1080p_30");
    Mlt::Producer decoded(profile, lossless.c_str());
    REQUIRE(decoded.is_valid());
    CHECK(decoded.get_length() == 15);
    EngineSync graph(model, PreviewScale::Full, EngineSync::FrameReads::ProfileSize);
    for (int i : {0, 5, 11, 14}) {
        INFO("rendered frame " << 25 + i);
        decoded.seek(i);
        std::unique_ptr<Mlt::Frame> frame(decoded.get_frame());
        mlt_image_format format = mlt_image_yuv422;
        int w = 1920, h = 1080;
        const uint8_t *image = frame->get_image(format, w, h);
        graph.tractor().seek(25 + i);
        std::unique_ptr<Mlt::Frame> reference(graph.tractor().get_frame());
        reference->set("consumer.rescale", "bilinear");
        mlt_image_format yuv = mlt_image_yuv422;
        int rw = 1920, rh = 1080;
        const uint8_t *expected = reference->get_image(yuv, rw, rh);
        long differing = 0;
        for (size_t k = 0; k < size_t{1920} * 1080 * 2; ++k)
            differing += image[k] != expected[k];
        CHECK(differing == 0);
    }

    std::ostringstream bad;
    CHECK(frames.run({project, "--range", "10:9999"}, bad) == 2);
    fs::remove_all(dir);
}
