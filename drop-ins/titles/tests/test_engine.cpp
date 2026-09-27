// Titles in the engine (doc 16, T1): libmltustudio from the curated module
// directory (IP4), a ustudio_title producer per title clip (IP3), elastic
// timing on the real graph, per-clip fields, reloading a changed file, and
// the same frames through u-studio-render (IP6).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/media/fingerprint.h"
#include "render/title_renderer.h"
#include "core/media/utf8_path.h"
#include "core/title_xml.h"
#include "core/xml/writer.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"
#include "engine/backdrop.h"
#include "engine/title_export.h"
#include "engine/title_extension.h"
#include "engine/title_frames.h"
#include "platform/process.h"

#include <cairo.h>
#include <glib.h>
#include <mlt++/Mlt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

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
    CHECK(mlt_properties_count(parameters) == 5);
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

TEST_CASE("dynamic fields: the producer draws what the renderer does, and redraws only when the text changes")
{
    setUp();
    const std::string path = writeTitle("dynamic.ustitle", R"(<ustitle version="1" width="640" height="360" fps="30/1">
      <timing intro="0" hold="120" outro="0"/>
      <layer kind="text" x="40" y="120" w="560"><text>{{timecode}} {{clip_time}} {{countdown:00:03}}</text>
        <font family="Sans" size="40"/><fill color="#ffffff"/></layer>
    </ustitle>)");
    Mlt::Profile profile;
    profile.set_width(640);
    profile.set_height(360);
    profile.set_frame_rate(30, 1);
    auto producer = titles::makeTitleProducer(profile, path, 120, {});
    REQUIRE(producer);
    producer->set("timeline_start", 30.0 * 3600); // the clip starts an hour in
    auto doc = titles::readTitle(path);
    REQUIRE(doc);
    for (int f : {0, 29, 30, 95}) {
        CAPTURE(f);
        producer->seek(f);
        std::unique_ptr<Mlt::Frame> frame(producer->get_frame());
        mlt_image_format format = mlt_image_rgba;
        int w = 640, h = 360;
        const uint8_t *image = frame->get_image(format, w, h);
        titles::FieldClock clock;
        clock.clipFrame = f;
        clock.timelineFrame = 30.0 * 3600 + f;
        clock.fps = 30;
        const titles::RenderResult reference = titles::renderTitle(doc->document, f, {}, 640, 360, &clock);
        std::vector<uint8_t> straight(640 * 360 * 4);
        titles::toStraightRgba(reference.frame, straight.data());
        CHECK(std::equal(straight.begin(), straight.end(), image));
    }
}

TEST_CASE("a title clip later in the sequence animates from its own start")
{
    // A playlist sets a frame's position to the sequence frame after the
    // producer made it; the producer must time the title by its own.
    setUp();
    const std::string path = writeTitle("later.ustitle");
    Scene early(path, 93), late(path, 93);
    late.model.removeClip(late.clip);
    late.clip = late.model.insertClip(late.upper, late.title, 90, 0, 92);
    engine::EngineSync a(early.model), b(late.model);
    for (int f : {0, 9, 40, 85}) {
        CAPTURE(f);
        CHECK(hashAt(a, f) == hashAt(b, 90 + f));
    }
}

TEST_CASE("{{timecode}} follows the clip's place in the sequence; {{clip_time}} doesn't")
{
    setUp();
    const auto sceneAt = [](const std::string &titlePath, FrameIndex position) {
        auto scene = std::make_unique<Scene>(titlePath, 93);
        scene->model.removeClip(scene->clip);
        scene->clip = scene->model.insertClip(scene->upper, scene->title, position, 0, 92);
        return scene;
    };
    const char *timecode = R"(<ustitle version="1" width="1920" height="1080" fps="30/1">
      <timing intro="0" hold="93" outro="0"/>
      <layer kind="text" x="100" y="800" w="900"><text>{{timecode}}</text><font family="Sans" size="64"/>
        <fill color="#ffffff"/></layer></ustitle>)";
    const std::string tcPath = writeTitle("timecode.ustitle", timecode);
    std::string clipTime = timecode;
    clipTime.replace(clipTime.find("{{timecode}}"), 12, "{{clip_time}}");
    const std::string ctPath = writeTitle("clip-time.ustitle", clipTime.c_str());

    auto tcEarly = sceneAt(tcPath, 0), tcLate = sceneAt(tcPath, 90);
    auto ctEarly = sceneAt(ctPath, 0), ctLate = sceneAt(ctPath, 90);
    engine::EngineSync a(tcEarly->model), b(tcLate->model), c(ctEarly->model), d(ctLate->model);
    // The same frame of each clip: 10 frames in.
    CHECK(hashAt(a, 10) != hashAt(b, 100));
    CHECK(hashAt(c, 10) == hashAt(d, 100));
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

// composite's 4:2:2 blend gave every half-transparent edge a dark,
// coloured fringe (red 60 where it should stay near 255); EngineSync now
// pairs the alpha of such sources first (engine_sync.cpp,
// attachAlphaPairing(); tests/engine/test_colour covers a PNG still).
TEST_CASE("anti-aliased edges over coloured video have no dark fringe")
{
    // White text over red: both have red at 255, so every edge pixel must
    // keep red near 255. A premultiplied value taken as straight (or the
    // reverse) darkens exactly these half-covered pixels.
    setUp();
    Scene scene(writeTitle("fringe.ustitle", R"(<ustitle version="1" width="1920" height="1080" fps="30/1">
      <timing intro="0" hold="60" outro="0"/>
      <layer kind="text" x="200" y="300" rotation="7"><text>Fringe Test</text><font family="Sans" size="160"/>
        <fill color="#ffffff"/></layer>
    </ustitle>)"),
                60);
    engine::EngineSync sync(scene.model);
    const std::vector<uint8_t> image = frameAt(sync, 30);
    int edges = 0, darkest = 255;
    for (int y = 280; y < 560; ++y)
        for (int x = 180; x < 1400; ++x) {
            const Rgba p = pixel(image, x, y);
            if (p.g > 10 && p.g < 245) { // partly covered: an edge
                ++edges;
                darkest = std::min(darkest, p.r);
            }
        }
    CHECK(edges > 500); // there are anti-aliased edges to judge
    CHECK(darkest >= 250);

    // And in an export. H.264's 4:2:0 shares one colour between four
    // pixels, which darkens a sharp red edge by itself, so this judges the
    // mean: 14 with the pairing, 31 without (2026-09-27).
    const fs::path rendered = scratch() / "fringe.mp4";
    std::string error;
    REQUIRE(engine::renderProject(scene.model, utf8String(rendered), error));
    Mlt::Profile profile("atsc_1080p_30");
    Mlt::Producer decoded(profile, utf8String(rendered).c_str());
    REQUIRE(decoded.is_valid());
    decoded.seek(30);
    std::unique_ptr<Mlt::Frame> frame(decoded.get_frame());
    mlt_image_format format = mlt_image_rgba;
    int w = 1920, h = 1080;
    const uint8_t *exported = frame->get_image(format, w, h);
    int exportedEdges = 0;
    double deficit = 0;
    for (int y = 280; y < 560; ++y)
        for (int x = 180; x < 1400; ++x) {
            const uint8_t *p = exported + (static_cast<size_t>(y) * 1920 + static_cast<size_t>(x)) * 4;
            if (p[1] > 10 && p[1] < 245) {
                ++exportedEdges;
                deficit += 255 - p[0];
            }
        }
    CHECK(exportedEdges > 500);
    CHECK(deficit / std::max(exportedEdges, 1) <= 22.0);
}

TEST_CASE("the designer's backdrop is the frame without the title, rendered off the main thread")
{
    setUp();
    Scene scene(writeTitle("backdrop.ustitle"), 150);
    engine::EngineSync sync(scene.model);
    Rgba withTitle = pixel(frameAt(sync, 60), 400, 950);
    CHECK(withTitle.g > 240); // the white bar
    titles::Backdrop backdrop;
    std::thread worker([&] { backdrop = titles::renderBackdrop(scene.model.snapshot(), scene.clip, 60); });
    worker.join();
    REQUIRE(backdrop.width == 1920);
    REQUIRE(backdrop.height == 1080);
    const uint8_t *p = backdrop.rgba.data() + (950 * 1920 + 400) * 4;
    CHECK(p[0] > 200); // red: the title isn't in it
    CHECK(p[1] < 50);
    // The project it was given is untouched.
    CHECK(scene.model.clip(scene.clip).videoEnabled);
}

TEST_CASE("animated frames from the producer are the renderer's own, byte for byte")
{
    // What the designer draws (renderTitle) is what the editor and export
    // get from ustudio_title: every behaviour, at every moment tried.
    setUp();
    const std::string path =
        writeTitle("behaviours.ustitle", R"(<ustitle version="1" width="640" height="360" fps="30/1">
      <timing intro="30" hold="60" outro="30"/>
      <layer kind="text" x="60" y="120" w="520"><text>Hello brave world</text><font family="Sans" size="48"/>
        <fill color="#ffffff"/>
        <behavior slot="in" id="kinetic-stack" duration="24"/><behavior slot="loop" id="wiggle" duration="10"/>
        <behavior slot="out" id="typewriter" duration="20"/></layer>
    </ustitle>)");
    Mlt::Profile profile;
    profile.set_width(640);
    profile.set_height(360);
    profile.set_frame_rate(30, 1);
    auto producer = titles::makeTitleProducer(profile, path, 120, {});
    REQUIRE(producer);
    auto doc = titles::readTitle(path);
    for (int f : {3, 12, 40, 61, 100, 115}) {
        CAPTURE(f);
        producer->seek(f);
        std::unique_ptr<Mlt::Frame> frame(producer->get_frame());
        mlt_image_format format = mlt_image_rgba;
        int w = 640, h = 360;
        const uint8_t *image = frame->get_image(format, w, h);
        const titles::RenderResult reference = titles::renderTitle(doc->document, f, {}, 640, 360);
        std::vector<uint8_t> straight(640 * 360 * 4);
        titles::toStraightRgba(reference.frame, straight.data());
        CHECK(std::equal(straight.begin(), straight.end(), image));
    }
}

TEST_CASE("a bake: the sequence's rate and length, and stock melt plays it with its alpha")
{
    setUp();
    const std::string path = writeTitle("bake.ustitle");
    titles::TitleExportRequest request;
    request.title = path;
    request.output = utf8String(scratch() / "bake (baked).mov");
    request.format = "prores";
    request.frames = 93;
    request.fps = core::Rational{25, 1};
    request.fields = {{"field.name", std::string("Ada"), {}}};
    std::atomic<bool> cancel{false};
    auto written = titles::exportTitle(request, &cancel);
    REQUIRE_MESSAGE(written.has_value(), (written ? "" : written.error()));
    CHECK(*written == 93);
    core::Profile profile;
    profile.fps = {25, 1};
    const auto probed = engine::EngineSync::probeMediaFile(profile, request.output);
    CHECK(probed.length == 93);
    CHECK(probed.width == 1920);

    // Cancelled: nothing left behind.
    titles::TitleExportRequest again = request;
    again.output = utf8String(scratch() / "cancelled.mov");
    again.frames = 3000;
    std::atomic<bool> stop{false};
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        stop.store(true);
    });
    auto stopped = titles::exportTitle(again, &stop);
    canceller.join();
    CHECK_FALSE(stopped.has_value());
    CHECK_FALSE(fs::exists(scratch() / "cancelled.mov"));
    CHECK_FALSE(fs::exists(scratch() / "cancelled.mov.part"));

#ifdef TITLES_MELT
    // Stock melt, without the titles module, renders frame 40 (the hold) to
    // a PNG: the bar is opaque white, the frame around it transparent.
    const fs::path png = scratch() / "melt-frame.png";
    fs::remove(png);
    std::vector<std::string> args = {TITLES_MELT, "-profile",   "atsc_1080p_25", request.output,
                                     "in=40",     "out=40",     "-consumer",     "avformat:" + utf8String(png),
                                     "f=image2",  "vcodec=png", "pix_fmt=rgba",  "mlt_image_format=rgba",
                                     "update=1"};
    std::vector<char *> argv;
    for (std::string &arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    // A plain environment: what a user's shell gives melt, and nothing a
    // sandboxed parent (a snap's LD paths) leaks into it.
    std::string home = std::string("HOME=") + g_get_home_dir();
    std::string pathVar = "PATH=/usr/bin:/bin";
    std::vector<char *> envp = {home.data(), pathVar.data(), nullptr};
    gint status = 0;
    REQUIRE(g_spawn_sync(nullptr, argv.data(), envp.data(),
                         static_cast<GSpawnFlags>(G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL), nullptr,
                         nullptr, nullptr, nullptr, &status, nullptr));
    CHECK(g_spawn_check_wait_status(status, nullptr));
    REQUIRE(fs::exists(png));
    cairo_surface_t *image = cairo_image_surface_create_from_png(utf8String(png).c_str());
    REQUIRE(cairo_surface_status(image) == CAIRO_STATUS_SUCCESS);
    const auto *data = cairo_image_surface_get_data(image);
    const int stride = cairo_image_surface_get_stride(image);
    const auto alphaAt = [&](int x, int y) { return data[y * stride + x * 4 + 3]; }; // ARGB32, little-endian
    CHECK(alphaAt(400, 880) == 255);                                                 // inside the bar
    CHECK(alphaAt(1500, 200) == 0);                                                  // nothing there
    cairo_surface_destroy(image);
#else
    MESSAGE("no melt: its check skipped");
#endif
}

TEST_CASE("--title-export: alpha formats keep the title's alpha; H.264 is flattened")
{
    setUp();
    const std::string title = writeTitle("export.ustitle", R"(<ustitle version="1" width="320" height="180" fps="25/1">
      <timing intro="0" hold="10" outro="0"/>
      <layer kind="shape" x="40" y="100" w="160" h="50"><fill color="#1b1230" opacity="0.5"/></layer>
    </ustitle>)");
    const auto readBack = [](const std::string &path, int x, int y) {
        Mlt::Profile profile;
        profile.set_width(320);
        profile.set_height(180);
        profile.set_frame_rate(25, 1);
        Mlt::Producer producer(profile, path.c_str());
        REQUIRE(producer.is_valid());
        producer.seek(5);
        std::unique_ptr<Mlt::Frame> frame(producer.get_frame());
        mlt_image_format format = mlt_image_rgba;
        int w = 320, h = 180;
        const uint8_t *image = frame->get_image(format, w, h);
        const uint8_t *p = image + (static_cast<size_t>(y) * 320 + static_cast<size_t>(x)) * 4;
        return Rgba{p[0], p[1], p[2], p[3]};
    };

    std::ostringstream out;
    const std::string mov = utf8String(scratch() / "export.mov");
    REQUIRE(titles::runTitleExport({title, mov, "qtrle"}, out) == 0);
    CHECK(out.str().find(R"("frames":10)") != std::string::npos);
    CHECK_FALSE(fs::exists(mov + ".part"));
    CHECK(readBack(mov, 20, 20).a == 0);                    // clear where the title is empty
    CHECK(std::abs(readBack(mov, 100, 120).a - 128) <= 2);  // half-transparent bar
    CHECK(std::abs(readBack(mov, 100, 120).r - 0x1b) <= 2); // in its own colour, not darkened

    const std::string mp4 = utf8String(scratch() / "export.mp4");
    REQUIRE(titles::runTitleExport({title, mp4, "h264", "--seconds", "0.4"}, out) == 0);
    CHECK(readBack(mp4, 20, 20).a == 255); // flattened: opaque everywhere
    CHECK(readBack(mp4, 20, 20).g < 20);   // on black

    const std::string frames = utf8String(scratch() / "export-frames");
    REQUIRE(titles::runTitleExport({title, frames, "png"}, out) == 0);
    size_t count = 0;
    for (const auto &entry : fs::directory_iterator(frames))
        count += entry.path().extension() == ".png";
    CHECK(count == 10);

    std::ostringstream bad;
    CHECK(titles::runTitleExport({title, mov, "gif"}, bad) == 2);
    CHECK(bad.str().find("unknown format") != std::string::npos);
    CHECK(titles::runTitleExport({utf8String(scratch() / "none.ustitle"), mov, "qtrle"}, bad) == 1);
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
    const gboolean spawned = g_spawn_sync(nullptr, argv.data(), env, G_SPAWN_STDERR_TO_DEV_NULL, nullptr, nullptr, &out,
                                          nullptr, &status, &error);
    g_strfreev(env);
    REQUIRE(spawned);
    CHECK(g_spawn_check_wait_status(status, nullptr));
    CHECK(std::string(out) == expected);
    g_free(out);
}
#endif
