// M5's gate, box 2 (doc 12): a keyframed transform, a masked effect, a
// dissolve with effects on both sides and an adjustment block render
// identically in the preview and in `u-studio-render` output. One synthetic
// project with all four: the live Engine's frames (the ones the preview
// widget shows) against the real render tool's `--frames` hashes of the
// saved project, the tool run as its own process with this drop-in in it.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/descriptor.h"
#include "core/media/frame_hash.h"
#include "core/media/utf8_path.h"
#include "core/model/model.h"
#include "core/xml/writer.h"
#include "dropins/api.h"
#include "dropins/dropin_host.h"
#include "dropins/registry.h"
#include "engine/engine.h"
#include "engine/engine_extension.h"
#include "engine/factory_policy.h"
#include "platform/process.h"

#include <glib.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <regex>
#include <string>
#include <thread>

using namespace ustudio;
using namespace ustudio::core;
using namespace ustudio::effects;
namespace fs = std::filesystem;

extern "C" const UStudioDropInDescription *ustudio_dropin_effects_describe(void);

namespace {

fs::path scratch()
{
    static const fs::path dir =
        fs::temp_directory_path() / ("ustudio-effects-render-" + std::to_string(platform::currentProcessId()));
    fs::create_directories(dir);
    return dir;
}

// The drop-in the way main() brings it up (test_engine.cpp's setUp()).
void setUp()
{
    static const bool done = [] {
        g_setenv("XDG_CACHE_HOME", (scratch() / "cache").c_str(), TRUE);
        dropins::DropInRegistry registry;
        registry.addBuiltin(ustudio_dropin_effects_describe());
        engine::FactoryPaths paths;
        registry.contributeFactoryPaths(paths);
        static engine::FactoryPolicy policy(paths);
        engine::clearEngineExtensions();
        static dropins::BasicDropInHost host("test");
        registry.registerAll(host);
        return true;
    }();
    (void)done;
}

Effect brightness(double level, std::vector<Keyframe> keys = {})
{
    Effect effect;
    effect.service = "brightness";
    effect.owner = kOwner;
    effect.params = {{"level", level, std::move(keys)}, {"rgb_only", true, {}}};
    return effect;
}

// V1: grey a [0, 100) dissolving over 10 frames into grey b [100, 200), a
// keyframed brightness on a, an ellipse-masked one on b. V2: a blue picture
// [40, 160), placed and keyframed (moving, growing and turning across the
// dissolve). An adjustment block over every track, [60, 140), faded in and
// out.
Model makeModel()
{
    Model model = Model::createEmpty();
    const TrackId v1 = model.addTrack(Track::Kind::Video, 0, "V1");
    const TrackId v2 = model.addTrack(Track::Kind::Video, 0, "V2");
    auto colour = [&](const char *path) {
        Asset asset;
        asset.path = path;
        asset.info.hasVideo = true;
        asset.info.lengthInSequenceFrames = 1000;
        return model.addAsset(asset);
    };
    const AssetId grey = colour("color:0x808080ff");
    const ClipId a = model.insertClip(v1, grey, 0, 100, 199);
    const ClipId b = model.insertClip(v1, grey, 100, 500, 599);
    REQUIRE(model.addTransition(v1, a, b, 5, 5).value != 0);
    model.addEffect(Model::EffectTarget::clip(a),
                    brightness(1.0, {{0, 0.4, Easing::CubicInOut}, {104, 1.6, Easing::Linear}}), 0);
    const EffectId masked = model.addEffect(Model::EffectTarget::clip(b), brightness(0.3), 0);
    EffectMask mask;
    mask.shape = "ellipse";
    mask.params = {{"x", 0.4, {}}, {"y", 0.5, {}}, {"width", 0.5, {}}, {"height", 0.6, {}}};
    mask.feather = {0.1, {}};
    model.setEffectMask(masked, mask);

    const ClipId blue = model.insertClip(v2, colour("color:0x2040c0ff"), 40, 0, 119);
    Transform placed;
    placed.bounds = Transform::Bounds::None;
    placed.x = {480, {{0, 480, Easing::CubicInOut}, {70, 1400, Easing::Linear}}};
    placed.y.value = 540;
    placed.width = {640, {{0, 640, Easing::Linear}, {70, 1100, Easing::Linear}}};
    placed.height = {360, {{0, 360, Easing::Linear}, {70, 620, Easing::Linear}}};
    placed.rotation = {0, {{10, 0, Easing::Linear}, {90, 40, Easing::SinusoidalOut}}};
    model.setClipTransform(blue, placed);

    AdjustmentBlock block;
    block.start = 60;
    block.length = 80;
    block.fadeIn = FadeSpec{15};
    block.fadeOut = FadeSpec{15};
    block.effects = {brightness(1.3)};
    model.addAdjustmentBlock(block);
    REQUIRE(model.check().empty());
    return model;
}

template <class Done> bool pumpUntil(Done done, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_iteration(nullptr, FALSE)) {
        }
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// `u-studio-render --frames <project>` as its own process: frame -> hash.
std::map<int, std::string> renderedHashes(const std::string &project)
{
    gchar **env = g_get_environ();
    env = g_environ_setenv(env, "XDG_CACHE_HOME", (scratch() / "render-cache").c_str(), TRUE);
#ifdef EFFECTS_DROPIN_MODULE_DIR
    // An uninstalled render tool finds the module only through this (ADR-014).
    env = g_environ_setenv(env, "USTUDIO_DROPIN_PATH", EFFECTS_DROPIN_MODULE_DIR, TRUE);
#endif
    const gchar *argv[] = {EFFECTS_RENDER_TOOL, "--frames", project.c_str(), nullptr};
    gchar *out = nullptr, *err = nullptr;
    gint status = 0;
    GError *error = nullptr;
    const bool ran = g_spawn_sync(nullptr, const_cast<gchar **>(argv), env, G_SPAWN_DEFAULT, nullptr, nullptr, &out,
                                  &err, &status, &error);
    g_strfreev(env);
    std::map<int, std::string> hashes;
    if (!ran || !g_spawn_check_wait_status(status, nullptr)) {
        MESSAGE("render tool failed: " << (error ? error->message : "") << (err ? err : ""));
        g_clear_error(&error);
    } else {
        const std::string json = out ? out : "";
        static const std::regex entry(R"re(\{"frame":(\d+),"hash":"([0-9a-f]{16})"\})re");
        for (auto it = std::sregex_iterator(json.begin(), json.end(), entry); it != std::sregex_iterator(); ++it)
            hashes[std::stoi((*it)[1])] = (*it)[2];
    }
    g_free(out);
    g_free(err);
    return hashes;
}

} // namespace

TEST_CASE("M5 gate: a keyframed transform, masks, a dissolve with effects and a block, preview = u-studio-render")
{
    setUp();
    const Model model = makeModel();
    const std::string project = utf8String(scratch() / "m5-box2.ustudio");
    REQUIRE(saveProject(model, project).empty());

    const std::map<int, std::string> rendered = renderedHashes(project);
    REQUIRE(rendered.size() == 200); // the whole sequence

    // The live preview: the Engine the editor runs, its frames as the widget gets them.
    engine::Engine engine(model.snapshot(), engine::PreviewScale::Full);
    std::mutex mutex;
    std::map<int, std::string> shown;
    engine.setFrameCallback([&](std::vector<uint8_t> rgba, int, int, int position) {
        std::lock_guard<std::mutex> lock(mutex);
        shown[position] = frameHash(rgba.data(), rgba.size());
    });
    // Before the transform, its first key, the block fading in, the
    // dissolve (both sides' effects, the mask, the moving picture), the
    // block at full, its fade out, the transform's last frame, after.
    for (int position : {0, 39, 40, 50, 65, 80, 94, 97, 100, 103, 110, 130, 139, 159, 160, 199}) {
        INFO("frame " << position);
        engine.seek(position);
        REQUIRE(pumpUntil(
            [&] {
                std::lock_guard<std::mutex> lock(mutex);
                return shown.contains(position);
            },
            std::chrono::seconds(20)));
        std::lock_guard<std::mutex> lock(mutex);
        REQUIRE(rendered.contains(position));
        CHECK(shown.at(position) == rendered.at(position));
    }
    // The picture really moves and turns: its frames differ along the way.
    CHECK(rendered.at(50) != rendered.at(80));
    engine.shutdown();
}
