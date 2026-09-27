// Titles in the engine (doc 16, T1): libmltustudio from the curated module
// directory (IP4), a ustudio_title producer per title clip (IP3), elastic
// timing on the real graph, per-clip fields, reloading a changed file, and
// the same frames through u-studio-render (IP6).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/media/fingerprint.h"
#include "core/media/utf8_path.h"
#include "core/title_xml.h"
#include "core/xml/writer.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"
#include "engine/title_extension.h"
#include "engine/title_frames.h"
#include "platform/process.h"

#include <glib.h>
#include <mlt++/Mlt.h>

#include <filesystem>
#include <map>
#include <memory>
#include <string>

using namespace ustudio;
using namespace ustudio::core;
namespace fs = std::filesystem;

namespace {

void setUp()
{
    static const bool done = [] {
        engine::FactoryPaths paths;
        paths.mltModuleDirs.push_back(TITLES_MLT_BUILD_DIR);
        static engine::FactoryPolicy policy(paths);
        engine::registerEngineExtension([] { return titles::makeTitleExtension(); });
        return true;
    }();
    (void)done;
}

fs::path scratch()
{
    static const fs::path dir =
        fs::temp_directory_path() / ("ustudio-titles-engine-" + std::to_string(platform::currentProcessId()));
    fs::create_directories(dir);
    return dir;
}

// A 30 fps title: a white bar that slides in over the intro (18 frames),
// holds, and fades out over the outro (15), with a {{name}} field.
constexpr const char *kTitle = R"(<ustitle version="1" width="1920" height="1080" fps="30/1">
  <timing intro="18" hold="60" outro="15"/>
  <field name="name" default="Jay Doe"/>
  <layer id="bar" kind="shape" x="100" y="800" w="600" h="160">
    <fill color="#ffffff"/>
    <animate property="x"><key at="0" value="-700" easing="cubic_out"/><key at="18" value="100"/></animate>
    <animate property="opacity"><key at="0" zone="outro" value="1"/><key at="15" zone="outro" value="0"/></animate>
  </layer>
  <layer id="name" kind="text" x="130" y="820" w="540" fit="shrink">
    <text>{{name}}</text><font family="Sans" weight="700" size="64"/><fill color="#1b1230"/>
  </layer>
</ustitle>)";

std::string writeTitle(const std::string &name, const char *xml = kTitle)
{
    auto doc = titles::parseTitle(xml);
    REQUIRE(doc.has_value());
    const fs::path path = scratch() / name;
    REQUIRE(titles::saveTitle(doc->document, utf8String(path)).empty());
    return utf8String(path);
}

// A red track under a title clip `length` frames long (a 30 fps sequence).
struct Scene
{
    Model model = Model::createEmpty();
    TrackId upper, lower;
    AssetId title;
    ClipId clip;

    Scene(const std::string &titlePath, FrameIndex length, std::vector<Param> fields = {})
    {
        lower = model.addTrack(Track::Kind::Video, 0, "V1");
        upper = model.addTrack(Track::Kind::Video, 0, "V2");
        Asset red;
        red.path = "color:red";
        red.info.hasVideo = true;
        red.info.lengthInSequenceFrames = 10'000;
        model.insertClip(lower, model.addAsset(red), 0, 0, 999);
        Asset asset;
        asset.path = titlePath;
        asset.displayName = "title";
        asset.info.hasVideo = true;
        asset.info.width = 1920;
        asset.info.height = 1080;
        asset.fileFingerprint = fileFingerprint(titlePath);
        title = model.addAsset(asset);
        clip = model.insertClip(upper, title, 0, 0, length - 1);
        if (!fields.empty())
            model.setClipSourceParams(clip, std::move(fields));
    }
};

struct Rgba
{
    int r, g, b, a;
};

// A frame's pixels at the sequence's size.
std::vector<uint8_t> frameAt(engine::EngineSync &sync, int position)
{
    Mlt::Tractor &tractor = sync.tractor();
    tractor.seek(position);
    std::unique_ptr<Mlt::Frame> frame(tractor.get_frame());
    mlt_image_format format = mlt_image_rgba;
    int w = sync.profile().width(), h = sync.profile().height();
    const uint8_t *image = frame->get_image(format, w, h);
    REQUIRE(format == mlt_image_rgba);
    return {image, image + static_cast<size_t>(w) * static_cast<size_t>(h) * 4};
}

Rgba pixel(const std::vector<uint8_t> &image, int x, int y)
{
    const uint8_t *p = image.data() + (static_cast<size_t>(y) * 1920 + static_cast<size_t>(x)) * 4;
    return {p[0], p[1], p[2], p[3]};
}

std::string hashAt(engine::EngineSync &sync, int position)
{
    const std::vector<uint8_t> image = frameAt(sync, position);
    return titles::frameHash(image.data(), image.size());
}

} // namespace

TEST_CASE("libmltustudio loads from the curated directory, with its metadata")
{
    setUp();
    mlt_properties meta = mlt_repository_metadata(mlt_factory_repository(), mlt_service_producer_type, "ustudio_title");
    REQUIRE(meta != nullptr);
    CHECK(std::string(mlt_properties_get(meta, "identifier")) == "ustudio_title");
    CHECK(std::string(mlt_properties_get(meta, "type")) == "producer");
    auto *parameters = static_cast<mlt_properties>(mlt_properties_get_data(meta, "parameters", nullptr));
    REQUIRE(parameters != nullptr);
    CHECK(mlt_properties_count(parameters) == 3);
    // Not a title: no producer, so the clip falls back to the placeholder.
    Mlt::Profile profile;
    CHECK(titles::makeTitleProducer(profile, "/nonexistent.ustitle", 10, {}) == nullptr);
}

TEST_CASE("a title clip plays over the track below it, with alpha")
{
    setUp();
    Scene scene(writeTitle("alpha.ustitle"), 150);
    engine::EngineSync sync(scene.model);
    CHECK(sync.extensionCount() == 1);
    // Frame 0: the bar is off-screen (x = -700); red shows below the text.
    Rgba start = pixel(frameAt(sync, 0), 400, 950);
    CHECK(start.r > 200);
    CHECK(start.g < 50);
    // Mid-hold: the white bar at (400, 880), red around it.
    const std::vector<uint8_t> hold = frameAt(sync, 60);
    Rgba bar = pixel(hold, 400, 950);
    CHECK(bar.r > 240);
    CHECK(bar.g > 240);
    CHECK(bar.b > 240);
    Rgba around = pixel(hold, 1200, 500);
    CHECK(around.r > 200);
    CHECK(around.g < 50);
    // The last frame: faded out to the red below.
    Rgba end = pixel(frameAt(sync, 149), 400, 950);
    CHECK(end.r > 200);
    CHECK(end.g < 60);
}

TEST_CASE("stretching the clip changes only the hold")
{
    setUp();
    const std::string path = writeTitle("elastic.ustitle");
    Scene shortScene(path, 93); // as designed
    Scene longScene(path, 400);
    engine::EngineSync shortSync(shortScene.model);
    engine::EngineSync longSync(longScene.model);
    for (int f : {0, 5, 9, 17}) {
        CAPTURE(f);
        CHECK(hashAt(shortSync, f) == hashAt(longSync, f)); // intro
    }
    for (int back : {1, 5, 10, 15}) {
        CAPTURE(back);
        CHECK(hashAt(shortSync, 93 - back) == hashAt(longSync, 400 - back)); // outro, from the end
    }
    CHECK(hashAt(longSync, 200) == hashAt(shortSync, 40)); // any hold frame is the hold
}

TEST_CASE("field values are the clip's own")
{
    setUp();
    const std::string path = writeTitle("fields.ustitle");
    Scene ada(path, 93, {{"field.name", std::string("Ada"), {}}});
    Scene grace(path, 93, {{"field.name", std::string("Grace"), {}}});
    Scene ada2(path, 93, {{"field.name", std::string("Ada"), {}}});
    Scene fallback(path, 93);
    engine::EngineSync a(ada.model), g(grace.model), a2(ada2.model), d(fallback.model);
    CHECK(hashAt(a, 40) != hashAt(g, 40));
    CHECK(hashAt(a, 40) == hashAt(a2, 40));
    CHECK(hashAt(d, 40) != hashAt(a, 40)); // "Jay Doe", the default
}

TEST_CASE("a changed file shows once the asset's fingerprint changes")
{
    setUp();
    const std::string path = writeTitle("reload.ustitle");
    Scene scene(path, 93);
    engine::EngineSync sync(scene.model);
    const std::string before = hashAt(sync, 40);
    // The same title in cyan: what the file watch sees after a save.
    std::string cyan = kTitle;
    cyan.replace(cyan.find("#ffffff"), 7, "#19e3ff");
    writeTitle("reload.ustitle", cyan.c_str());
    sync.setProject(std::make_shared<const Project>(scene.model.project())); // unchanged: no rebuild
    CHECK(hashAt(sync, 40) == before);
    scene.model.setAssetSource(scene.title, path, fileFingerprint(path), Asset::Status::Ready);
    sync.setProject(std::make_shared<const Project>(scene.model.project()));
    CHECK(hashAt(sync, 40) != before);
    Rgba bar = pixel(frameAt(sync, 40), 400, 950);
    CHECK(bar.r < 60);
    CHECK(bar.g > 200);
}

TEST_CASE("a missing or broken title file doesn't stop the rest playing")
{
    setUp();
    const std::string broken = utf8String(scratch() / "broken.ustitle");
    {
        std::FILE *f = std::fopen(broken.c_str(), "w");
        std::fputs("<ustitle version=", f);
        std::fclose(f);
    }
    for (const std::string &path : {broken, utf8String(scratch() / "gone.ustitle")}) {
        CAPTURE(path);
        Scene scene(path, 60);
        engine::EngineSync sync(scene.model);
        Rgba below = pixel(frameAt(sync, 30), 1200, 500);
        CHECK(below.a == 255);
    }
}

#ifdef TITLES_RENDER_TOOL
TEST_CASE("u-studio-render plays the title with identical frames (T1 acceptance)")
{
    setUp();
    const std::string path = writeTitle("render.ustitle");
    Scene scene(path, 120, {{"field.name", std::string("Ada Lovelace"), {}}});
    const std::string project = utf8String(scratch() / "render.ustudio");
    REQUIRE(saveProject(scene.model, project).empty());
    engine::EngineSync sync(scene.model);
    const std::vector<int> frames = {0, 9, 18, 60, 110, 119};
    std::string expected = R"({"width":1920,"height":1080,"frames":[)";
    for (size_t i = 0; i < frames.size(); ++i)
        expected += std::string(i ? "," : "") + R"({"frame":)" + std::to_string(frames[i]) + R"(,"hash":")" +
                    hashAt(sync, frames[i]) + R"("})";
    expected += "]}\n";

    std::vector<std::string> args = {TITLES_RENDER_TOOL, "--title-frames", project};
    for (int f : frames)
        args.push_back(std::to_string(f));
    std::vector<char *> argv;
    for (std::string &arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    gchar **env = g_get_environ();
#ifdef TITLES_DROPIN_MODULE_DIR
    env = g_environ_setenv(env, "USTUDIO_DROPIN_PATH", TITLES_DROPIN_MODULE_DIR, TRUE);
#endif
    gchar *out = nullptr;
    gint status = -1;
    GError *error = nullptr;
    const gboolean spawned = g_spawn_sync(nullptr, argv.data(), env, G_SPAWN_STDERR_TO_DEV_NULL, nullptr, nullptr,
                                          &out, nullptr, &status, &error);
    g_strfreev(env);
    REQUIRE(spawned);
    CHECK(g_spawn_check_wait_status(status, nullptr));
    CHECK(std::string(out) == expected);
    g_free(out);
}
#endif
