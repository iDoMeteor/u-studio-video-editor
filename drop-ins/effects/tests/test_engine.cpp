// The effects drop-in against real MLT (doc 15, FX1): the registry, frei0r
// curation, effects on clips (dissolve tails and heads too), tracks and the
// sequence, the mix, in-place value changes, the quarantine, preview ==
// export == melt, and no Qt with every frei0r plugin loaded.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/blocks.h"
#include "core/commands.h"
#include "core/commands/undo_stack.h"
#include "core/media/utf8_path.h"
#include "core/model/model.h"
#include "core/xml/writer.h"
#include "dropins/api.h"
#include "dropins/dropin_host.h"
#include "dropins/registry.h"
#include "engine/effects_extension.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"
#include "engine/frame_renderer.h"
#include "core/looks.h"
#include "engine/plugins.h"
#include "engine/probe.h"
#include "engine/registry.h"
#include "platform/process.h"

#include <glib.h>
#include <mlt++/Mlt.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

using namespace ustudio;
using namespace ustudio::core;
using namespace ustudio::effects;
namespace fs = std::filesystem;

extern "C" const UStudioDropInDescription *ustudio_dropin_effects_describe(void);

namespace {

fs::path scratch()
{
    static const fs::path dir =
        fs::temp_directory_path() / ("ustudio-effects-engine-" + std::to_string(platform::currentProcessId()));
    fs::create_directories(dir);
    return dir;
}

// The drop-in the way main() brings it up: its factory paths into
// FactoryPolicy, then registered. The cache (Qt scan, health file) goes to
// this test's scratch directory, not the user's.
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

// Grey 0x808080 from 0 to 99 and from 100 to 199 with a 10-frame dissolve
// between them (clip a extended into it), on V1 over the black background.
struct Timeline
{
    Model model = Model::createEmpty();
    TrackId track;
    ClipId a, b;
    Timeline()
    {
        track = model.addTrack(Track::Kind::Video, 0, "V1");
        Asset asset;
        asset.path = "color:0x808080ff";
        asset.info.hasVideo = true;
        asset.info.lengthInSequenceFrames = 1000;
        AssetId id = model.addAsset(asset);
        a = model.insertClip(track, id, 0, 100, 199);
        b = model.insertClip(track, id, 100, 500, 599);
        model.addTransition(track, a, b, 5, 5);
    }
    // brightness (MLT's; level 0 black, 1 unchanged, 2 double) owned by us.
    // rgb_only: without it brightness scales YUV luma, and 0.5 of grey 128
    // reads 54, not 64 (filter_brightness.yml).
    EffectId addBrightness(Model::EffectTarget target, double level, std::vector<Keyframe> keys = {},
                           KeyframedValue mix = {1.0, {}})
    {
        Effect effect;
        effect.service = "brightness";
        effect.owner = kOwner;
        Param param;
        param.name = "level";
        param.value = level;
        param.keyframes = std::move(keys);
        Param rgbOnly;
        rgbOnly.name = "rgb_only";
        rgbOnly.value = true;
        effect.params = {param, rgbOnly};
        effect.mix = std::move(mix);
        return model.addEffect(target, effect, 0);
    }
};

// Red at the frame's centre, from a tractor pulled at 64x36.
int redAt(Mlt::Producer &producer, int position)
{
    producer.seek(position);
    std::unique_ptr<Mlt::Frame> frame(producer.get_frame());
    mlt_image_format format = mlt_image_rgba;
    int w = 64, h = 36;
    const uint8_t *image = frame->get_image(format, w, h);
    return image[(static_cast<size_t>(h / 2) * static_cast<size_t>(w) + static_cast<size_t>(w / 2)) * 4];
}

bool near(int value, int expected, int tolerance = 6)
{
    return std::abs(value - expected) <= tolerance;
}

constexpr int kGrey = 128;

} // namespace

TEST_CASE("Registry: every MLT filter but the GPU engine's, normalised, overlaid, cached")
{
    setUp();
    Mlt::Repository repository(mlt_factory_repository());
    const EffectRegistry registry = EffectRegistry::scan(repository, loadOverlays(effectsDataDir() / "overlays"));
    CHECK(registry.descriptors().size() > 400);
    for (const EffectDescriptor &d : registry.descriptors())
        CHECK_MESSAGE(d.family != "movit", d.service);

    const EffectDescriptor *glow = registry.find("frei0r.glow");
    REQUIRE(glow);
    CHECK(glow->family == "frei0r");
    CHECK(glow->category == "Light"); // the overlay
    CHECK(glow->featured);
    REQUIRE(!glow->params.empty());
    CHECK(glow->params[0].id == "0"); // frei0r parameters by index (FX0)
    CHECK(glow->params[0].kind == ParamKind::Scalar);
    CHECK(glow->params[0].animatable);

    const EffectDescriptor *brightness = registry.find("brightness");
    REQUIRE(brightness);
    auto level = std::find_if(brightness->params.begin(), brightness->params.end(),
                              [](const ParamDescriptor &p) { return p.id == "level"; });
    REQUIRE(level != brightness->params.end());
    CHECK(level->hasDefault); // MLT gives none; the overlay's 1
    CHECK(std::get<double>(level->defaultValue) == 1.0);

    // Plumbing stays described (a project may name it) but hidden.
    for (const char *plumbing : {"mask_start", "mask_apply", "affine", "resize", "avcolour_space"})
        if (const EffectDescriptor *d = registry.find(plumbing))
            CHECK_MESSAGE(d->hidden, plumbing);

    // Known broken: marked by the overlay, quarantined from the start.
    if (const EffectDescriptor *flippo = registry.find("frei0r.3dflippo")) {
        CHECK(!flippo->unstable.empty());
        CHECK(isQuarantined("frei0r.3dflippo"));
    }

    // frei0r's not_thread_safe.txt: flagged, still offered (MLT serialises them).
    if (const EffectDescriptor *baltan = registry.find("frei0r.baltan")) {
        CHECK(baltan->notThreadSafe);
        CHECK(!baltan->hidden);
    }

    const std::string fingerprint = registryFingerprint();
    CHECK(fingerprint == registryFingerprint());
    std::optional<EffectRegistry> cached =
        EffectRegistry::fromJson(*parseJson(toJson(registry.toJson(fingerprint))), fingerprint);
    REQUIRE(cached);
    REQUIRE(cached->descriptors().size() == registry.descriptors().size());
    for (size_t i = 0; i < registry.descriptors().size(); ++i)
        CHECK_MESSAGE(cached->descriptors()[i] == registry.descriptors()[i], registry.descriptors()[i].service);
    CHECK(!EffectRegistry::fromJson(registry.toJson(fingerprint), "another plugin set"));
}

TEST_CASE("frei0r curation: a plugin naming Qt, or quarantined, never reaches FREI0R_PATH")
{
    setUp();
    const fs::path dir = scratch() / "fake-frei0r";
    fs::create_directories(dir);
    auto write = [&](const char *name, const std::string &content) {
        std::ofstream(dir / name, std::ios::binary) << content;
    };
    write("clean.so", std::string("\x7f"
                                  "ELF ... libc.so.6 ...",
                                  20));
    // The needle split across the reader's 64 KiB chunks must still count.
    write("qtish.so", std::string(65536 - 2, 'x') + "libQt6Core.so.6");
    write("crashy.so", "\x7f"
                       "ELF");
    write("readme.txt", "not a plugin");
    const std::vector<Frei0rPlugin> plugins = findFrei0rPlugins({dir});
    REQUIRE(plugins.size() == 3);
    CHECK(mentionsQt(dir / "qtish.so"));
    CHECK(!mentionsQt(dir / "clean.so"));

    HealthFile health;
    health.records["frei0r.crashy"] = {HealthStatus::Crashed, "died on frame 0", 0.0};
    const fs::path qtCache = scratch() / "qt-cache.json";
    Frei0rCuration curation = curateFrei0r(plugins, health, qtCache);
    REQUIRE(!curation.curatedDir.empty());
    CHECK(curation.paths == std::vector<std::string>{curation.curatedDir.string()});
    CHECK(fs::exists(curation.curatedDir / "clean.so"));
    CHECK(!fs::exists(curation.curatedDir / "qtish.so"));
    CHECK(!fs::exists(curation.curatedDir / "crashy.so"));
    CHECK(curation.excluded.size() == 2);
    CHECK((fs::status(curation.curatedDir).permissions() & fs::perms::others_all) == fs::perms::none);
    CHECK(fs::exists(qtCache)); // read once per file change

    // Nothing to leave out: the system directories as they are, no links.
    Frei0rCuration clean = curateFrei0r({plugins[0]}, {}, qtCache);
    CHECK(clean.curatedDir.empty());
    CHECK(!clean.paths.empty());

    // A later directory wins a name, as in MLT's frei0r factory.
    const fs::path later = scratch() / "fake-frei0r-later";
    fs::create_directories(later);
    std::ofstream(later / "clean.so", std::ios::binary) << "newer";
    const std::vector<Frei0rPlugin> both = findFrei0rPlugins({dir, later});
    auto clean2 = std::find_if(both.begin(), both.end(), [](const Frei0rPlugin &p) { return p.name == "clean"; });
    REQUIRE(clean2 != both.end());
    CHECK(clean2->file.parent_path() == later);
    CHECK(pluginSetFingerprint(both, "x") != pluginSetFingerprint(plugins, "x"));
}

TEST_CASE("Clip effects play on every cut, animated continuously through the dissolve")
{
    setUp();
    Timeline t;
    // level 0 -> 1 across a's 100 frames: frame f shows grey * f / 100.
    t.addBrightness(Model::EffectTarget::clip(t.a), 1.0, {{0, 0.0, Easing::Linear}, {100, 1.0, Easing::Linear}});
    extensionStats().reset();
    engine::EngineSync sync(t.model);
    CHECK(sync.verify().empty());
    CHECK(extensionStats().attached >= 2); // a's own cut and its dissolve tail
    Mlt::Tractor &tractor = sync.tractor();
    CHECK(redAt(tractor, 0) < 8);
    CHECK(near(redAt(tractor, 50), kGrey / 2));
    CHECK(near(redAt(tractor, 94), kGrey * 94 / 100));
    // Inside the dissolve (95..104) a's tail still ramps: no jump back to 0
    // or to full at the boundary.
    const int before = redAt(tractor, 94), inside = redAt(tractor, 96);
    CHECK(inside >= before - 6);
    CHECK(inside <= kGrey + 6);
}

TEST_CASE("Track and master effects, the mix, and effects that aren't ours")
{
    setUp();
    SUBCASE("a track effect covers the whole track")
    {
        Timeline t;
        t.addBrightness(Model::EffectTarget::track(t.track), 0.5);
        engine::EngineSync sync(t.model);
        CHECK(near(redAt(sync.tractor(), 20), kGrey / 2));
        CHECK(near(redAt(sync.tractor(), 150), kGrey / 2));
    }
    SUBCASE("a master effect covers the output")
    {
        Timeline t;
        t.addBrightness(Model::EffectTarget::sequence(), 0.5);
        engine::EngineSync sync(t.model);
        CHECK(near(redAt(sync.tractor(), 150), kGrey / 2));
    }
    SUBCASE("a 50% mix is halfway between the effect and none")
    {
        Timeline t;
        // level 0 (black) at 50%: grey halves.
        t.addBrightness(Model::EffectTarget::clip(t.a), 0.0, {}, {0.5, {}});
        engine::EngineSync sync(t.model);
        CHECK(near(redAt(sync.tractor(), 20), kGrey / 2));
        CHECK(near(redAt(sync.tractor(), 150), kGrey)); // b has none
    }
    SUBCASE("a keyframed mix animates")
    {
        Timeline t;
        t.addBrightness(Model::EffectTarget::clip(t.a), 0.0, {},
                        {1.0, {{0, 0.0, Easing::Linear}, {80, 1.0, Easing::Linear}}});
        engine::EngineSync sync(t.model);
        CHECK(near(redAt(sync.tractor(), 0), kGrey));
        CHECK(near(redAt(sync.tractor(), 40), kGrey / 2));
        CHECK(redAt(sync.tractor(), 80) < 8);
    }
    SUBCASE("another drop-in's effect, a disabled one, and a quarantined one don't play")
    {
        Timeline t;
        EffectId other = t.addBrightness(Model::EffectTarget::clip(t.a), 0.0);
        Effect foreign = t.model.effect(other);
        t.model.removeEffect(other);
        foreign.owner = "someone-else";
        t.model.addEffect(Model::EffectTarget::clip(t.a), foreign, 0);
        EffectId off = t.addBrightness(Model::EffectTarget::track(t.track), 0.0);
        t.model.setEffectEnabled(off, false);
        Effect glow;
        glow.service = "frei0r.glow";
        glow.owner = kOwner;
        t.model.addEffect(Model::EffectTarget::sequence(), glow, 0);
        setQuarantinedServices({"frei0r.glow"});
        extensionStats().reset();
        engine::EngineSync sync(t.model);
        setQuarantinedServices({});
        CHECK(near(redAt(sync.tractor(), 20), kGrey));
        CHECK(extensionStats().skipped >= 2); // the foreign one and the quarantined one
    }
    SUBCASE("a service MLT doesn't have plays nothing, and says so once")
    {
        Timeline t;
        Effect missing;
        missing.service = "frei0r.not-installed-here";
        missing.owner = kOwner;
        missing.mix = {0.5, {}};
        t.model.addEffect(Model::EffectTarget::clip(t.a), missing, 0);
        engine::EngineSync sync(t.model);
        CHECK(near(redAt(sync.tractor(), 20), kGrey));
    }
}

TEST_CASE("A slider drag applies in place: no rebuild, so the consumer never restarts")
{
    setUp();
    Timeline t;
    EffectId id = t.addBrightness(Model::EffectTarget::clip(t.a), 1.0, {}, {0.8, {}});
    engine::EngineSync sync(t.model);
    int rebuilds = 0, inPlace = 0;
    sync.rebuilt.connect([&] { ++rebuilds; });
    sync.appliedInPlace.connect([&] { ++inPlace; });
    extensionStats().reset();

    // Thirty drag steps through the command, as the Rack will send them.
    UndoStack undo(t.model);
    for (int step = 0; step <= 30; ++step) {
        Param level;
        level.name = "level";
        level.value = 1.0 - step / 30.0; // to black
        undo.execute(std::make_unique<SetParam>(id, level, 1));
        sync.setProject(t.model.snapshot());
    }
    CHECK(rebuilds == 0);
    CHECK(inPlace == 30); // the first step is the value it already had: nothing to apply
    // At 80% mix of black: 20% of grey.
    CHECK(near(redAt(sync.tractor(), 20), kGrey / 5));

    // The mix moves in place too while it stays below 1 ...
    undo.execute(std::make_unique<SetMix>(id, KeyframedValue{0.5, {}}, 2));
    sync.setProject(t.model.snapshot());
    CHECK(rebuilds == 0);
    CHECK(near(redAt(sync.tractor(), 20), kGrey / 2));
    // ... but reaching a constant 1 drops the mask pair: a rebuild.
    undo.execute(std::make_unique<SetMix>(id, KeyframedValue{1.0, {}}, 3));
    sync.setProject(t.model.snapshot());
    CHECK(rebuilds == 1);
    CHECK(redAt(sync.tractor(), 20) < 8);

    // Each gesture is one undo step: three undos back to the start.
    for (int i = 0; i < 3; ++i)
        CHECK(undo.undo());
    CHECK(!undo.canUndo());
    CHECK(std::get<double>(t.model.effect(id).params[0].value) == 1.0);
}

TEST_CASE("Preview equals export equals melt: the same frames from every path")
{
    setUp();
    Timeline t;
    // A keyframed clip effect across the dissolve, a mixed frei0r effect on
    // b, a track effect and a master effect.
    t.addBrightness(Model::EffectTarget::clip(t.a), 1.0, {{0, 0.3, Easing::CubicInOut}, {104, 1.5, Easing::Linear}});
    Effect glow;
    glow.service = "frei0r.glow";
    glow.owner = kOwner;
    Param blur;
    blur.name = "0";
    blur.value = 0.4;
    glow.params = {blur};
    glow.mix = {0.6, {}};
    t.model.addEffect(Model::EffectTarget::clip(t.b), glow, 0);
    t.addBrightness(Model::EffectTarget::track(t.track), 0.9);
    t.addBrightness(Model::EffectTarget::sequence(), 1.1);

    const std::vector<int> frames = {0, 40, 94, 97, 100, 103, 110, 180};
    engine::EngineSync preview(t.model);
    // Export's graph: renderProject() builds its own EngineSync at full
    // size, reading frames at the profile's (engine_sync.cpp).
    engine::EngineSync render(t.model, engine::PreviewScale::Full, engine::EngineSync::FrameReads::ProfileSize);
    const fs::path path = scratch() / "effects.ustudio";
    REQUIRE(saveProject(t.model, utf8String(path)).empty());
    Mlt::Profile profile("atsc_1080p_30");
    Mlt::Producer melt(profile, ("xml:" + utf8String(path)).c_str());
    REQUIRE(melt.is_valid());
    for (int frame : frames) {
        INFO("frame " << frame);
        const int p = redAt(preview.tractor(), frame);
        CHECK(p == redAt(render.tractor(), frame));
        CHECK(near(p, redAt(melt, frame), 2));
    }

#ifdef EFFECTS_MELT
    // Stock melt renders the saved project (clip, track and master effects,
    // the mixed frei0r one included) to the end without failing.
    // "xml:": melt picks a loader by extension, and doesn't know .ustudio.
    std::vector<std::string> args = {EFFECTS_MELT, "xml:" + utf8String(path), "-consumer", "null", "real_time=0"};
    std::vector<char *> argv;
    for (std::string &arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    // A plain environment: what a user's shell gives melt.
    std::string home = std::string("HOME=") + g_get_home_dir();
    std::string pathVar = "PATH=/usr/bin:/bin";
    std::vector<char *> envp = {home.data(), pathVar.data(), nullptr};
    gint status = 0;
    gchar *errors = nullptr;
    REQUIRE(g_spawn_sync(nullptr, argv.data(), envp.data(), G_SPAWN_STDOUT_TO_DEV_NULL, nullptr, nullptr, nullptr,
                         &errors, &status, nullptr));
    const std::string stderrText = errors ? errors : "";
    INFO(stderrText);
    CHECK(g_spawn_check_wait_status(status, nullptr));
    g_free(errors);
#endif
}

TEST_CASE("The probe: ok with a cost for a working effect, unavailable for a missing one")
{
    setUp();
    const HealthRecord glow = probeEffect("frei0r.glow", 4);
    CHECK(glow.status == HealthStatus::Ok);
    CHECK(glow.msPerFrame >= 0.0);
    CHECK(probeEffect("frei0r.not-installed-here", 4).status == HealthStatus::Unavailable);
    std::vector<std::string> stages;
    probeEffect("brightness", 3, [&](const char *stage) { stages.emplace_back(stage); });
    CHECK(stages == std::vector<std::string>{"defaults", "minimums", "maximums", "half size"});
}

TEST_CASE("No Qt mapped with every frei0r plugin loaded")
{
    setUp();
    Mlt::Repository repository(mlt_factory_repository());
    std::unique_ptr<Mlt::Properties> filters(repository.filters());
    Mlt::Profile profile("atsc_1080p_30");
    // Creating a frei0r filter dlopens its plugin (MLT's frei0r factory), so
    // a Qt dependency would be mapped from here on. No frames: a plugin that
    // crashes is the probe's to find, in its own process (frei0r's 3dflippo
    // does, at any read size but the profile's).
    std::vector<std::unique_ptr<Mlt::Filter>> created;
    for (int i = 0; i < filters->count(); ++i) {
        const std::string service = filters->get_name(i);
        if (familyOf(service) != "frei0r")
            continue;
        auto filter = std::make_unique<Mlt::Filter>(profile, service.c_str());
        if (filter->is_valid())
            created.push_back(std::move(filter));
    }
    CHECK(created.size() > 50);
    std::ifstream maps("/proc/self/maps");
    std::string line;
    bool qt = false;
    while (std::getline(maps, line))
        qt = qt || line.find("libQt") != std::string::npos;
    CHECK_FALSE(qt);
}

namespace {

FrameRequest greyRequest(std::vector<Effect> effects)
{
    FrameRequest request;
    request.resource = "color:0x808080ff";
    request.sourceFrame = 10;
    request.clipIn = 0;
    request.clipOut = 99;
    request.effects = std::move(effects);
    request.width = 64;
    request.height = 36;
    return request;
}

Effect brightness(double level)
{
    Effect effect;
    effect.service = "brightness";
    effect.owner = kOwner;
    Param p;
    p.name = "level";
    p.value = level;
    Param rgb;
    rgb.name = "rgb_only";
    rgb.value = true;
    effect.params = {p, rgb};
    return effect;
}

int centreRed(const RenderedFrame &frame)
{
    return frame.rgba[(static_cast<size_t>(frame.height / 2) * static_cast<size_t>(frame.width) +
                       static_cast<size_t>(frame.width / 2)) *
                      4];
}

} // namespace

TEST_CASE("FrameRenderer: a clip's frame through a stack, off the live graph")
{
    setUp();
    const RenderedFrame plain = FrameRenderer::renderNow(greyRequest({}));
    REQUIRE(plain.width == 64);
    REQUIRE(plain.height == 36);
    CHECK(near(centreRed(plain), kGrey));
    CHECK(near(centreRed(FrameRenderer::renderNow(greyRequest({brightness(0.5)}))), kGrey / 2));
    // The stack in order, as the editor plays it.
    CHECK(near(centreRed(FrameRenderer::renderNow(greyRequest({brightness(0.5), brightness(0.5)}))), kGrey / 4));
    CHECK(FrameRenderer::renderNow(greyRequest({})).rgba == plain.rgba);
    CHECK(greyRequest({brightness(0.5)}).key() != greyRequest({brightness(0.6)}).key());
}

TEST_CASE("FrameRenderer: results on the main loop, cached, stale generations dropped")
{
    setUp();
    FrameRenderer renderer;
    GMainContext *context = g_main_context_default();
    auto waitFor = [&](const bool &flag) {
        for (int i = 0; i < 400 && !flag; ++i) {
            g_main_context_iteration(context, FALSE);
            g_usleep(5000);
        }
    };

    bool done = false;
    int red = -1;
    renderer.request(greyRequest({brightness(0.5)}), 1, 1, [&](const RenderedFrame &frame) {
        red = centreRed(frame);
        done = true;
    });
    waitFor(done);
    REQUIRE(done);
    CHECK(near(red, kGrey / 2));
    REQUIRE(renderer.cached(greyRequest({brightness(0.5)})));

    // From the cache: at once, on the calling thread.
    bool again = false;
    renderer.request(greyRequest({brightness(0.5)}), 1, 1, [&](const RenderedFrame &) { again = true; });
    CHECK(again);

    // A newer generation on the lane makes the older ones moot: once 5 is
    // asked for, 2-4 arriving after it are dropped unrendered.
    int delivered = 0;
    bool newest = false;
    renderer.request(greyRequest({brightness(0.5 * 1.5)}), 1, 5, [&](const RenderedFrame &) {
        ++delivered;
        newest = true;
    });
    for (uint64_t generation = 2; generation < 5; ++generation)
        renderer.request(greyRequest({brightness(0.1 * static_cast<double>(generation))}), 1, generation,
                         [&](const RenderedFrame &) { ++delivered; });
    waitFor(newest);
    CHECK(newest);
    for (int i = 0; i < 40; ++i) { // any strays would arrive now
        g_main_context_iteration(context, FALSE);
        g_usleep(5000);
    }
    CHECK(delivered == 1);

    // Idle, the worker closes its media (an open decoder is its largest
    // cost: about 180 MB for 1080p H.264), and opens it again when asked.
    CHECK(renderer.holdsMedia());
    const auto idleSince = std::chrono::steady_clock::now();
    while (renderer.holdsMedia() && std::chrono::steady_clock::now() - idleSince < std::chrono::seconds(10))
        g_usleep(50000);
    CHECK_FALSE(renderer.holdsMedia());
    CHECK(std::chrono::steady_clock::now() - idleSince >= FrameRenderer::kReleaseIdleMedia / 2);
    bool reopened = false;
    renderer.request(greyRequest({brightness(0.3)}), 1, 6,
                     [&](const RenderedFrame &frame) { reopened = !frame.rgba.empty(); });
    waitFor(reopened);
    CHECK(reopened);
    CHECK(renderer.holdsMedia());

    // A clip that doesn't open, asked for twice (the second time from the
    // same open slot): an empty frame both times, never a seek on it.
    for (int attempt = 0; attempt < 2; ++attempt) {
        FrameRequest missing = greyRequest({brightness(0.2 + 0.1 * attempt)});
        missing.resource = (scratch() / "no-such-clip.mp4").string();
        bool answered = false, empty = false;
        renderer.request(std::move(missing), 1, 7 + static_cast<uint64_t>(attempt), [&](const RenderedFrame &frame) {
            answered = true;
            empty = frame.rgba.empty();
        });
        waitFor(answered);
        CHECK(answered);
        CHECK(empty);
    }

    // At the application's shutdown every renderer stops (FrameRenderer::
    // stopAll()), before main() closes the factory; its own stop() later is
    // a no-op.
    FrameRenderer::stopAll();
    CHECK_FALSE(renderer.holdsMedia());
    renderer.stop();
    bool afterStop = false;
    renderer.request(greyRequest({brightness(0.9)}), 1, 9, [&](const RenderedFrame &) { afterStop = true; });
    CHECK(!afterStop);
}

TEST_CASE("Brand looks: every one ships with services this install has, and changes the picture")
{
    setUp();
    std::ifstream in(effectsDataDir() / "looks" / "brand.json");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::optional<Json> json = parseJson(text);
    REQUIRE(json);
    const std::vector<Look> looks = looksFromJson(*json);
    REQUIRE(looks.size() >= 5);
    const RenderedFrame plain = FrameRenderer::renderNow(greyRequest({}));
    for (const Look &look : looks) {
        INFO(look.name);
        Mlt::Profile profile("atsc_1080p_30");
        for (const Effect &effect : look.effects) {
            Mlt::Filter filter(profile, effect.service.c_str());
            CHECK_MESSAGE(filter.is_valid(), effect.service);
        }
        // Each changes a flat grey frame somewhere: a colour shift, a
        // vignette's corners, grain.
        const RenderedFrame frame = FrameRenderer::renderNow(greyRequest(effectsOf(look)));
        REQUIRE(!frame.rgba.empty());
        CHECK(frame.rgba != plain.rgba);
    }
}

namespace {

// Red at (u, v) (fractions of the frame), pulled at 160x90.
int redAt(Mlt::Producer &producer, int position, double u, double v)
{
    producer.seek(position);
    std::unique_ptr<Mlt::Frame> frame(producer.get_frame());
    mlt_image_format format = mlt_image_rgba;
    int w = 160, h = 90;
    const uint8_t *image = frame->get_image(format, w, h);
    const auto x = static_cast<size_t>(u * w), y = static_cast<size_t>(v * h);
    return image[(y * static_cast<size_t>(w) + x) * 4];
}

EffectMask centredMask(const std::string &shape, bool invert = false)
{
    EffectMask mask;
    mask.shape = shape;
    mask.params = {{"x", 0.5, {}}, {"y", 0.5, {}}, {"width", 0.5, {}}, {"height", 0.5, {}}};
    mask.feather = {0.0, {}};
    mask.invert = invert;
    return mask;
}

} // namespace

// FX4: a mask limits an effect to a shape (frei0r.alphaspot between
// mask_start and mask_apply; core::nativeFilters()).
TEST_CASE("A mask limits an effect to its shape, in the editor and in melt")
{
    setUp();
    Timeline t;
    const EffectId black = t.addBrightness(Model::EffectTarget::clip(t.a), 0.0);
    SUBCASE("rectangle: black inside, grey outside")
    {
        t.model.setEffectMask(black, centredMask("rectangle"));
        engine::EngineSync sync(t.model);
        CHECK(redAt(sync.tractor(), 20, 0.5, 0.5) < 8);
        CHECK(redAt(sync.tractor(), 20, 0.3, 0.3) < 8); // inside the rectangle's corner
        CHECK(near(redAt(sync.tractor(), 20, 0.05, 0.05), kGrey));
        CHECK(near(redAt(sync.tractor(), 20, 0.9, 0.5), kGrey));
    }
    SUBCASE("inverted: the other way round")
    {
        t.model.setEffectMask(black, centredMask("rectangle", true));
        engine::EngineSync sync(t.model);
        CHECK(near(redAt(sync.tractor(), 20, 0.5, 0.5), kGrey));
        CHECK(redAt(sync.tractor(), 20, 0.05, 0.05) < 8);
    }
    SUBCASE("ellipse: a rectangle's corner is outside it")
    {
        t.model.setEffectMask(black, centredMask("ellipse"));
        engine::EngineSync sync(t.model);
        CHECK(redAt(sync.tractor(), 20, 0.5, 0.5) < 8);
        CHECK(near(redAt(sync.tractor(), 20, 0.27, 0.27), kGrey));
    }
    SUBCASE("a 50% mix inside the shape, and melt plays the saved file the same")
    {
        t.model.setEffectMask(black, centredMask("rectangle"));
        t.model.setEffectMix(black, {0.5, {}});
        engine::EngineSync sync(t.model);
        CHECK(near(redAt(sync.tractor(), 20, 0.5, 0.5), kGrey / 2));
        const fs::path path = scratch() / "mask.ustudio";
        REQUIRE(saveProject(t.model, utf8String(path)).empty());
        Mlt::Producer melt(sync.profile(), ("xml:" + utf8String(path)).c_str());
        REQUIRE(melt.is_valid());
        for (auto [u, v] : {std::pair{0.5, 0.5}, {0.05, 0.05}, {0.3, 0.3}})
            CHECK(near(redAt(melt, 20, u, v), redAt(sync.tractor(), 20, u, v), 3));
    }
}

TEST_CASE("A masked effect's mix, invert and geometry apply in place; a new shape rebuilds")
{
    setUp();
    Timeline t;
    const EffectId black = t.addBrightness(Model::EffectTarget::clip(t.a), 0.0);
    t.model.setEffectMask(black, centredMask("rectangle"));
    engine::EngineSync sync(t.model);
    int rebuilds = 0, inPlace = 0;
    sync.rebuilt.connect([&] { ++rebuilds; });
    sync.appliedInPlace.connect([&] { ++inPlace; });
    UndoStack undo(t.model);
    auto apply = [&](std::unique_ptr<core::Command> command) {
        REQUIRE(undo.execute(std::move(command)));
        sync.setProject(t.model.snapshot());
    };

    // The mix lives in alphaspot's inside alpha now, not the transition.
    apply(std::make_unique<SetMix>(black, KeyframedValue{0.5, {}}));
    CHECK(near(redAt(sync.tractor(), 20, 0.5, 0.5), kGrey / 2));
    CHECK(near(redAt(sync.tractor(), 20, 0.05, 0.05), kGrey));
    // Inverted: the mix moves to the outside alpha.
    EffectMask inverted = centredMask("rectangle", true);
    apply(std::make_unique<SetEffectMask>(black, inverted));
    CHECK(near(redAt(sync.tractor(), 20, 0.5, 0.5), kGrey));
    CHECK(near(redAt(sync.tractor(), 20, 0.05, 0.05), kGrey / 2));
    // The shape moved left: its old middle is outside now (effect there).
    inverted.params = {{"x", 0.2, {}}, {"y", 0.5, {}}, {"width", 0.2, {}}, {"height", 0.5, {}}};
    apply(std::make_unique<SetEffectMask>(black, inverted));
    CHECK(near(redAt(sync.tractor(), 20, 0.5, 0.5), kGrey / 2));
    CHECK(near(redAt(sync.tractor(), 20, 0.2, 0.5), kGrey));
    CHECK(rebuilds == 0);
    CHECK(inPlace == 3);
    // Another shape: other filter values chosen at build time: a rebuild.
    inverted.shape = "ellipse";
    apply(std::make_unique<SetEffectMask>(black, inverted));
    CHECK(rebuilds == 1);
    // And one the editor can't draw is refused by the model's check.
    Model broken = t.model;
    inverted.shape = "polygon";
    broken.setEffectMask(black, inverted);
    CHECK_FALSE(broken.check().empty());
}

// FX5: a LUT library item (Catalog "lut:<path>") is avfilter.lut3d with the
// file; a generated .cube (text, no binary media) that maps everything to
// red turns the grey clip red, in the editor and in melt.
TEST_CASE("A LUT from the library grades the clip, in the editor and in melt")
{
    setUp();
    const fs::path cube = scratch() / "all-red.cube";
    {
        std::ofstream out(cube);
        out << "TITLE \"all red\"\nLUT_3D_SIZE 2\n";
        for (int i = 0; i < 8; ++i)
            out << "1.0 0.0 0.0\n";
    }
    Timeline t;
    Effect lut;
    lut.service = "avfilter.lut3d";
    lut.owner = kOwner;
    lut.params = {{"av.file", utf8String(cube), {}}, {"av.interp", std::string("tetrahedral"), {}}};
    t.model.addEffect(Model::EffectTarget::clip(t.a), lut, 0);
    engine::EngineSync sync(t.model);
    CHECK(redAt(sync.tractor(), 20) > 240);
    CHECK(near(redAt(sync.tractor(), 150), kGrey)); // b has none
    const fs::path path = scratch() / "lut.ustudio";
    REQUIRE(saveProject(t.model, utf8String(path)).empty());
    Mlt::Producer melt(sync.profile(), ("xml:" + utf8String(path)).c_str());
    REQUIRE(melt.is_valid());
    CHECK(redAt(melt, 20) > 240);
}

// FX5: LADSPA and VST2 plugins are curated like frei0r's (MLT's jackrack
// host opens every .so under LADSPA_PATH/VST_PATH, recursively, at start).
TEST_CASE("LADSPA and VST2 curation: a plugin naming Qt never reaches LADSPA_PATH; VST2 off unless chosen")
{
    setUp();
    const fs::path dir = scratch() / "fake-ladspa";
    fs::create_directories(dir / "pack");
    std::ofstream(dir / "clean.so", std::ios::binary) << std::string("\x7f" "ELF libc.so.6", 13);
    std::ofstream(dir / "pack" / "qtish.so", std::ios::binary) << "\x7f" "ELF libQt6Widgets.so.6";
    std::ofstream(dir / "pack" / "clean.so", std::ios::binary) << "\x7f" "ELF also clean";
    std::ofstream(dir / "readme.txt", std::ios::binary) << "not a plugin";
    const char *remembered = std::getenv("USTUDIO_LADSPA_SEARCH_PATH");
    const std::string saved = remembered ? remembered : "";
    g_setenv("USTUDIO_LADSPA_SEARCH_PATH", utf8String(dir).c_str(), TRUE);

    CHECK(audioSearchDirs(AudioHost::Ladspa) == std::vector<fs::path>{dir});
    CHECK(audioHostFiles(AudioHost::Ladspa).size() == 3); // recursive, .so only
    const PluginCuration ladspa = curateAudioHost(AudioHost::Ladspa, true, scratch() / "audio-qt.json");
    REQUIRE(!ladspa.curatedDir.empty());
    CHECK(ladspa.paths == std::vector<std::string>{ladspa.curatedDir.string()});
    CHECK(ladspa.excluded.size() == 1);
    // Both clean files, the second renamed (names meet in one flat folder).
    CHECK(fs::exists(ladspa.curatedDir / "clean.so"));
    CHECK(fs::exists(ladspa.curatedDir / "clean-2.so"));
    CHECK(!fs::exists(ladspa.curatedDir / "qtish.so"));
    CHECK((fs::status(ladspa.curatedDir).permissions() & fs::perms::others_all) == fs::perms::none);

    // VST2 without its preference: a folder that isn't there.
    const PluginCuration vst = curateAudioHost(AudioHost::Vst2, false, scratch() / "audio-qt.json");
    REQUIRE(vst.paths.size() == 1);
    CHECK(!fs::exists(vst.paths[0]));

    if (remembered)
        g_setenv("USTUDIO_LADSPA_SEARCH_PATH", saved.c_str(), TRUE);
    else
        g_unsetenv("USTUDIO_LADSPA_SEARCH_PATH");
}

TEST_CASE("OpenFX: a bundle naming Qt, at the top or one folder deep, is found")
{
    setUp();
    const fs::path dir = scratch() / "fake-ofx";
    auto bundle = [&](const fs::path &at, const std::string &content) {
        const fs::path binary = at / "Contents" / "Linux-x86-64";
        fs::create_directories(binary);
        std::ofstream(binary / (at.stem().stem().string() + ".ofx"), std::ios::binary) << content;
    };
    bundle(dir / "Clean.ofx.bundle", "\x7f" "ELF libc.so.6");
    CHECK(openfxBundlesNamingQt({dir}).empty());
    bundle(dir / "vendor" / "Qtish.ofx.bundle", "\x7f" "ELF libQt5Core.so.5");
    bundle(dir / "vendor" / "deeper" / "Deep.ofx.bundle", "\x7f" "ELF libQt6Gui.so.6"); // MLT doesn't look there
    const std::vector<fs::path> naming = openfxBundlesNamingQt({dir});
    REQUIRE(naming.size() == 1);
    CHECK(naming[0].filename() == "Qtish.ofx.bundle");
    // The fixed folders are always searched, OFX_PLUGIN_PATH after them.
    const std::vector<fs::path> dirs = openfxSearchDirs();
    REQUIRE(dirs.size() >= 2);
    CHECK(dirs[0] == "/usr/OFX/Plugins");
}

// FX4: an adjustment block on lane 0 (above every track) puts its effects
// on the output for exactly its range, its fades ramping them, in the
// editor and in melt; a value dragged in place keeps the fades.
TEST_CASE("An adjustment block affects exactly its range, fades in, and plays the same in melt")
{
    setUp();
    Timeline t; // grey, the whole timeline [0, 190)
    AdjustmentBlock block;
    block.lane = 0;
    block.start = 120;
    block.length = 40; // [120, 160)
    block.fadeIn = FadeSpec{10};
    Effect black;
    black.service = "brightness";
    black.owner = kOwner;
    black.params = {{"level", 0.0, {}}, {"rgb_only", true, {}}};
    block.effects = {black};
    const AdjustmentBlockId id = t.model.addAdjustmentBlock(block);
    REQUIRE(t.model.check().empty());
    engine::EngineSync sync(t.model);
    CHECK(near(redAt(sync.tractor(), 119), kGrey));      // before it
    CHECK(near(redAt(sync.tractor(), 125), kGrey / 2));  // half-way through its fade
    CHECK(redAt(sync.tractor(), 140) < 8);              // inside
    CHECK(near(redAt(sync.tractor(), 160), kGrey));      // after it
    const fs::path path = scratch() / "block.ustudio";
    REQUIRE(saveProject(t.model, utf8String(path)).empty());
    Mlt::Producer melt(sync.profile(), ("xml:" + utf8String(path)).c_str());
    REQUIRE(melt.is_valid());
    for (int frame : {119, 125, 140, 160})
        CHECK(near(redAt(melt, frame), redAt(sync.tractor(), frame), 3));

    // Its effect's value, dragged: in place, the fade still there.
    int rebuilds = 0, inPlace = 0;
    sync.rebuilt.connect([&] { ++rebuilds; });
    sync.appliedInPlace.connect([&] { ++inPlace; });
    const EffectId effect = t.model.adjustmentBlock(id).effects[0].id;
    UndoStack undo(t.model);
    REQUIRE(undo.execute(std::make_unique<SetParam>(effect, Param{"level", 0.5, {}}, 1)));
    sync.setProject(t.model.snapshot());
    CHECK(rebuilds == 0);
    CHECK(inPlace == 1);
    CHECK(near(redAt(sync.tractor(), 140), kGrey / 2));
    CHECK(near(redAt(sync.tractor(), 125), kGrey * 3 / 4)); // half the fade of half the effect

    // A fade-handle drag: in place too, the ramp following the new length.
    for (FrameIndex fade : {20, 30})
        REQUIRE(undo.execute(std::make_unique<SetAdjustmentBlockFades>(id, FadeSpec{fade}, std::nullopt, 2)));
    sync.setProject(t.model.snapshot());
    CHECK(rebuilds == 0);
    CHECK(inPlace == 2);
    // At 125, 5 frames into a 30-frame fade: a sixth of half the effect.
    CHECK(near(redAt(sync.tractor(), 125), kGrey - kGrey / 12));
    CHECK(near(redAt(sync.tractor(), 155), kGrey / 2)); // past the fade

    // The first fade on a fadeless block, and the last one removed, change
    // its effects' filters (a plain filter against the mix wrap): rebuilds,
    // with the right picture each time (VE Core's case).
    REQUIRE(undo.execute(std::make_unique<SetAdjustmentBlockFades>(id, std::nullopt, std::nullopt)));
    sync.setProject(t.model.snapshot());
    REQUIRE(undo.execute(std::make_unique<SetParam>(effect, Param{"level", 0.0, {}}, 3)));
    sync.setProject(t.model.snapshot());
    const int before = rebuilds;
    CHECK(redAt(sync.tractor(), 125) < 8); // no fade: black from the block's start
    REQUIRE(undo.execute(std::make_unique<SetAdjustmentBlockFades>(id, FadeSpec{10}, std::nullopt)));
    sync.setProject(t.model.snapshot());
    CHECK(rebuilds == before + 1);
    CHECK(near(redAt(sync.tractor(), 125), kGrey / 2));
    REQUIRE(undo.execute(std::make_unique<SetAdjustmentBlockFades>(id, std::nullopt, std::nullopt)));
    sync.setProject(t.model.snapshot());
    CHECK(rebuilds == before + 2);
    CHECK(redAt(sync.tractor(), 125) < 8);
}

namespace {

// A whole frame, hashed (FNV-1a over its RGBA at the profile's size).
uint64_t frameHash(Mlt::Producer &producer, int position, int width, int height)
{
    producer.seek(position);
    std::unique_ptr<Mlt::Frame> frame(producer.get_frame());
    mlt_image_format format = mlt_image_rgba;
    int w = width, h = height;
    const uint8_t *image = frame->get_image(format, w, h);
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < static_cast<size_t>(w) * static_cast<size_t>(h) * 4; ++i)
        hash = (hash ^ image[i]) * 1099511628211ull;
    return hash;
}

} // namespace

// M5's gate (doc 12): a keyframed effect (a keyframed transform isn't in
// the model yet: ADR-018's transforms are single values), a masked effect,
// a dissolve with effects on both sides and an adjustment block give the
// same frames in the preview's graph and the export's (renderProject()
// builds its own EngineSync at full size), hash for hash; melt plays the
// saved project within rounding. (Exports are H.264 until M6, so the
// encoded file can't be compared hash for hash.)
TEST_CASE("M5 gate: preview and export give the same frames for effects, masks, a dissolve and a block")
{
    setUp();
    Timeline t; // a [0, 100) and b [100, 200), a 10-frame dissolve across 100
    t.addBrightness(Model::EffectTarget::clip(t.a), 1.0, {{0, 0.4, Easing::CubicInOut}, {104, 1.6, Easing::Linear}});
    const EffectId masked = t.addBrightness(Model::EffectTarget::clip(t.b), 0.3);
    EffectMask mask;
    mask.shape = "ellipse";
    mask.params = {{"x", 0.4, {}}, {"y", 0.5, {}}, {"width", 0.5, {}}, {"height", 0.6, {}}};
    mask.feather = {0.1, {}};
    t.model.setEffectMask(masked, mask);
    AdjustmentBlock block;
    block.start = 60;
    block.length = 80; // across the dissolve
    block.fadeIn = FadeSpec{15};
    block.fadeOut = FadeSpec{15};
    Effect grade;
    grade.service = "brightness";
    grade.owner = kOwner;
    grade.params = {{"level", 1.3, {}}, {"rgb_only", true, {}}};
    block.effects = {grade};
    t.model.addAdjustmentBlock(block);
    REQUIRE(t.model.check().empty());

    engine::EngineSync preview(t.model, engine::PreviewScale::Full, engine::EngineSync::FrameReads::ProfileSize);
    engine::EngineSync render(t.model, engine::PreviewScale::Full, engine::EngineSync::FrameReads::ProfileSize);
    CHECK(preview.verify().empty());
    const int w = preview.profile().width(), h = preview.profile().height();
    const fs::path path = scratch() / "m5-gate.ustudio";
    REQUIRE(saveProject(t.model, utf8String(path)).empty());
    Mlt::Producer melt(preview.profile(), ("xml:" + utf8String(path)).c_str());
    REQUIRE(melt.is_valid());
    for (int frame : {0, 50, 65, 94, 97, 100, 103, 110, 130, 150, 190}) {
        INFO("frame " << frame);
        CHECK(frameHash(preview.tractor(), frame, w, h) == frameHash(render.tractor(), frame, w, h));
        // Both read at the profile's size, as the preview's graph is read.
        auto red = [&](Mlt::Producer &producer, double u, double v) {
            producer.seek(frame);
            std::unique_ptr<Mlt::Frame> f(producer.get_frame());
            mlt_image_format format = mlt_image_rgba;
            int fw = w, fh = h;
            const uint8_t *image = f->get_image(format, fw, fh);
            return static_cast<int>(image[(static_cast<size_t>(v * fh) * static_cast<size_t>(fw) +
                                           static_cast<size_t>(u * fw)) * 4]);
        };
        INFO("melt " << red(melt, 0.4, 0.5) << " preview " << red(preview.tractor(), 0.4, 0.5));
        CHECK(near(red(melt, 0.4, 0.5), red(preview.tractor(), 0.4, 0.5), 3));
        CHECK(near(red(melt, 0.9, 0.1), red(preview.tractor(), 0.9, 0.1), 3));
    }
}
