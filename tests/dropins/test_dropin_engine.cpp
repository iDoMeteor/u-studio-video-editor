// IP3 (doc 15): EngineSync with the test drop-in's EngineExtension, built in
// and as a module. Every hook runs where doc 15 says, effects play animated
// per cut (dissolve tails and heads included), a value change is applied in
// place without a rebuild, and with no extension nothing changes.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/model/model.h"
#include "core/xml/reader.h"
#include "core/xml/writer.h"
#include "dropins/registry.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"
#include "engine/gpu_session.h"
#include "engine/test_extension.h"
#include "platform/process.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>

using namespace ustudio;
using namespace ustudio::core;
using namespace ustudio::engine;

extern "C" const UStudioDropInDescription *ustudio_dropin_testdropin_describe(void);

namespace {

FactoryPolicy &sharedFactoryPolicy()
{
    static FactoryPolicy policy;
    return policy;
}

// The drop-in, registered the way main() does.
void registerTestDropIn()
{
    clearEngineExtensions();
    testdropin::extensionLog() = {};
    dropins::DropInRegistry registry;
    registry.addBuiltin(ustudio_dropin_testdropin_describe());
    dropins::BasicDropInHost host("test");
    registry.registerAll(host);
}

// Red 0..99 then red 200..299 (a dissolve of 10 between them), a brightness
// effect on the first clip ramping 0 -> 1 over its first 100 frames.
struct Timeline
{
    Model model = Model::createEmpty();
    TrackId track;
    ClipId a, b;
    EffectId fade;
    Timeline(const std::string &owner = "testdropin")
    {
        track = model.addTrack(Track::Kind::Video, 0, "V1");
        Asset asset;
        asset.path = "color:red";
        asset.info.hasVideo = true;
        asset.info.lengthInSequenceFrames = 1000;
        AssetId id = model.addAsset(asset);
        a = model.insertClip(track, id, 0, 100, 199);
        b = model.insertClip(track, id, 100, 500, 599);
        model.addTransition(track, a, b, 5, 5);
        Effect effect;
        effect.service = "brightness";
        effect.owner = owner;
        Param level;
        level.name = "level";
        level.value = 1.0;
        level.keyframes = {{0, 0.0, Easing::Linear}, {100, 1.0, Easing::Linear}};
        effect.params = {level};
        fade = model.addEffect(Model::EffectTarget::clip(a), effect, 0);
    }
};

// color:red's red channel through a timeline graph. It measured 233 until
// 0.52.0-beta.2: a colour's YUV then went through the graph's BT.709 tag
// mismatch (docs/developer/notes/engine-sync.md); full red is 255.
constexpr int kFullRed = 255;

bool isNear(int red, int expected)
{
    return std::abs(red - expected) <= 16;
}

int redAt(EngineSync &sync, int position)
{
    Mlt::Tractor &tractor = sync.tractor();
    tractor.seek(position);
    std::unique_ptr<Mlt::Frame> frame(tractor.get_frame());
    mlt_image_format format = mlt_image_rgb;
    int w = 64, h = 36;
    const uint8_t *image = frame->get_image(format, w, h);
    return image[(static_cast<size_t>(h / 2) * static_cast<size_t>(w) + static_cast<size_t>(w / 2)) * 3];
}

} // namespace

TEST_CASE("IP3: with no extension the graph is built exactly as before")
{
    sharedFactoryPolicy();
    clearEngineExtensions();
    Timeline t;
    EngineSync sync(t.model);
    CHECK(sync.extensionCount() == 0);
    CHECK(sync.verify().empty());
    CHECK(isNear(redAt(sync, 0), kFullRed)); // the effect's drop-in isn't here: plays without it
}

TEST_CASE("IP3: decorateCut runs on every cut with its offset, and the effect plays animated through a dissolve")
{
    sharedFactoryPolicy();
    registerTestDropIn();
    Timeline t;
    EngineSync sync(t.model);
    REQUIRE(sync.extensionCount() == 1);
    CHECK(sync.verify().empty());
    // a is 105 frames once extended into the dissolve: its exclusive cut
    // (95 frames from its start) and its tail (10 frames, 95 in); b's head
    // (10 frames, 0 in) and b's exclusive rest.
    const auto &cuts = testdropin::extensionLog().cuts;
    CHECK(std::find(cuts.begin(), cuts.end(), std::make_pair<FrameIndex, FrameIndex>(0, 95)) != cuts.end());
    CHECK(std::find(cuts.begin(), cuts.end(), std::make_pair<FrameIndex, FrameIndex>(95, 10)) != cuts.end());
    CHECK(std::find(cuts.begin(), cuts.end(), std::make_pair<FrameIndex, FrameIndex>(0, 10)) != cuts.end());
    CHECK(testdropin::extensionLog().playlists == 1);
    CHECK(testdropin::extensionLog().tractors == 1);

    // brightness level 0 -> 1 across a's first 100 frames (MLT positions a
    // cut's filter animation from the cut's own start: frame 0 black).
    // Unfiltered red reads kFullRed through this graph, not 255.
    CHECK(redAt(sync, 0) < 10);
    CHECK(isNear(redAt(sync, 50), kFullRed / 2));
    CHECK(isNear(redAt(sync, 94), kFullRed * 94 / 100));
    CHECK(isNear(redAt(sync, 97), kFullRed * 97 / 100)); // inside the dissolve, on a's tail: still animated
}

TEST_CASE("IP3: a value change is applied in place (no rebuild); anything else rebuilds")
{
    sharedFactoryPolicy();
    registerTestDropIn();
    Timeline t;
    EngineSync sync(t.model);
    int rebuilds = 0, inPlace = 0;
    sync.rebuilt.connect([&] { ++rebuilds; });
    sync.appliedInPlace.connect([&] { ++inPlace; });

    Param level;
    level.name = "level";
    level.value = 0.5; // constant half brightness
    t.model.setEffectParam(t.fade, level);
    sync.setProject(t.model.snapshot());
    CHECK(rebuilds == 0);
    CHECK(inPlace == 1);
    CHECK(isNear(redAt(sync, 0), kFullRed / 2));
    CHECK(testdropin::extensionLog().inPlace == 1);

    // A structural change (a second effect) rebuilds.
    Effect another;
    another.service = "brightness";
    another.owner = "testdropin";
    t.model.addEffect(Model::EffectTarget::clip(t.b), another, 0);
    sync.setProject(t.model.snapshot());
    CHECK(rebuilds == 1);
    CHECK(inPlace == 1);
}

TEST_CASE("IP3: makeProducer, makeTransitionSegment and compositor replace the defaults when asked")
{
    sharedFactoryPolicy();
    registerTestDropIn();
    Timeline t;
    // b is generated by the drop-in: blue.
    Param colour;
    colour.name = "color";
    colour.value = std::string("#0000ff");
    t.model.setClipSourceParams(t.b, {colour});
    // The dissolve's recipe is a hard cut: b's head only.
    t.model.setTransitionRecipe(t.model.sequence().transitions[0].id, "hard-cut", {});
    testdropin::useTestCompositor() = true;
    EngineSync sync(t.model);
    testdropin::useTestCompositor() = false;

    CHECK(testdropin::extensionLog().producers == 1);
    CHECK(testdropin::extensionLog().recipes == 1);
    CHECK(testdropin::extensionLog().compositors == 1);
    CHECK(sync.verify().empty()); // b's resource isn't its asset's: skipped
    CHECK(redAt(sync, 97) < 10);  // inside the "dissolve": b alone, blue
    CHECK(redAt(sync, 150) < 10); // b, generated blue
}

TEST_CASE("IP3: the test drop-in built as a module plays its effect too")
{
    sharedFactoryPolicy();
    clearEngineExtensions();
    dropins::DropInRegistry registry;
    registry.loadModules({TEST_MODULE_DIR});
    REQUIRE(registry.entries().size() == 1);
    dropins::BasicDropInHost host("test");
    registry.registerAll(host);

    Timeline t("moduledropin"); // the module's name, which owns its effects
    EngineSync sync(t.model);
    CHECK(sync.extensionCount() == 1);
    CHECK(redAt(sync, 0) < 10);
    CHECK(isNear(redAt(sync, 94), kFullRed * 94 / 100));
    clearEngineExtensions();
}

TEST_CASE("IP3: decorateLane gets lane 0's adjustment blocks in start order, once per build")
{
    sharedFactoryPolicy();
    registerTestDropIn();
    Timeline t;
    for (FrameIndex start : {150, 20}) {
        AdjustmentBlock block;
        block.lane = 0;
        block.start = start;
        block.length = 30;
        t.model.addAdjustmentBlock(block);
    }
    AdjustmentBlock deeper; // lane 1: its sub-tractor comes with FX4, not called yet
    deeper.lane = 1;
    deeper.start = 60;
    deeper.length = 10;
    t.model.addAdjustmentBlock(deeper);
    EngineSync sync(t.model);
    const auto &lanes = testdropin::extensionLog().lanes;
    REQUIRE(lanes.size() == 1);
    CHECK(lanes[0].first == 0);
    CHECK(lanes[0].second == std::vector<FrameIndex>{20, 150});
}

namespace {

// A red picture with a 440 Hz tone, 60 frames, as a file (a video clip with
// its own sound), made with MLT.
std::string redWithTone()
{
    static const std::string path = [] {
        const std::string out = (std::filesystem::temp_directory_path() /
                                 ("ustudio-lane-" + std::to_string(platform::currentProcessId()) + ".mkv"))
                                    .string();
        Mlt::Profile profile("atsc_1080p_30");
        Mlt::Tractor tractor(profile);
        Mlt::Producer red(profile, "color:red"), tone(profile, "tone:");
        red.set("mlt_image_format", "rgba");
        red.set_in_and_out(0, 59);
        tone.set_in_and_out(0, 59);
        tractor.set_track(red, 0);
        tractor.set_track(tone, 1);
        Mlt::Transition mix(profile, "mix");
        mix.set("start", 1.0);
        mix.set("sum", 1);
        mix.set("always_active", 1);
        std::unique_ptr<Mlt::Field> field(tractor.field());
        field->plant_transition(mix, 0, 1);
        Mlt::Consumer consumer(profile, "avformat", out.c_str());
        consumer.set("vcodec", "ffv1");
        consumer.set("acodec", "pcm_s16le");
        consumer.set("real_time", -1);
        consumer.connect(tractor);
        consumer.run();
        return out;
    }();
    return path;
}

struct LaneScene
{
    Model model = Model::createEmpty();
    TrackId top, bottom;
    LaneScene()
    {
        bottom = model.addTrack(Track::Kind::Video, 0, "V1");
        top = model.addTrack(Track::Kind::Video, 0, "V2"); // row 0
        Asset red;
        red.path = redWithTone();
        red.status = Asset::Status::Ready;
        red.info.hasVideo = red.info.hasAudio = true;
        red.info.width = 1920;
        red.info.height = 1080;
        red.info.lengthInSequenceFrames = 60;
        model.insertClip(bottom, model.addAsset(red), 0, 0, 59);
        Asset blue;
        blue.path = "color:blue";
        blue.info.hasVideo = true;
        blue.info.lengthInSequenceFrames = 10'000;
        const ClipId picture = model.insertClip(top, model.addAsset(blue), 0, 0, 59);
        Transform placed; // a quarter-size picture in the top-left quarter
        placed.bounds = Transform::Bounds::None;
        placed.x.value = 480;
        placed.y.value = 270;
        placed.width.value = 960;
        placed.height.value = 540;
        model.setClipTransform(picture, placed);
    }
    void addBlock(int lane)
    {
        AdjustmentBlock block;
        block.lane = lane;
        block.start = 10;
        block.length = 20;
        Effect dark;
        dark.service = "brightness";
        dark.owner = "testdropin";
        Param level;
        level.name = "level";
        level.value = 0.0;
        dark.params = {level};
        block.effects = {dark};
        model.addAdjustmentBlock(block);
    }
};

struct Sample
{
    int r, g, b;
    double rms;
};

Sample sampleAt(EngineSync &sync, int position, int x, int y)
{
    sync.tractor().seek(position);
    std::unique_ptr<Mlt::Frame> frame(sync.tractor().get_frame());
    mlt_image_format format = mlt_image_rgb;
    frame->set("consumer.rescale", "bilinear");
    int fw = 1920, fh = 1080;
    const uint8_t *image = frame->get_image(format, fw, fh);
    const uint8_t *px = image + (static_cast<size_t>(y) * 1920 + static_cast<size_t>(x)) * 3;
    mlt_audio_format audioFormat = mlt_audio_s16;
    int frequency = 48000, channels = 2, samples = 1600;
    const auto *pcm = static_cast<const int16_t *>(frame->get_audio(audioFormat, frequency, channels, samples));
    double sum = 0;
    for (int i = 0; pcm && i < samples * channels; ++i)
        sum += static_cast<double>(pcm[i]) * pcm[i];
    return {px[0], px[1], px[2], std::sqrt(sum / (samples * channels))};
}

} // namespace

TEST_CASE("FX4: a block below the first row darkens only the rows beneath it, and they keep their sound")
{
    sharedFactoryPolicy();
    registerTestDropIn();
    LaneScene plain;
    EngineSync without(plain.model);
    const Sample before = sampleAt(without, 15, 1600, 900); // red, where the blue picture isn't

    LaneScene scene;
    scene.addBlock(1); // above row 1 (V1): V1 and below, not V2
    EngineSync sync(scene.model);
    CHECK(sync.verify().empty());
    const auto &lanes = testdropin::extensionLog().lanes;
    REQUIRE(!lanes.empty());
    CHECK(lanes.back().first == 1);

    const Sample inBlockRed = sampleAt(sync, 15, 1600, 900);
    CHECK(inBlockRed.r < 30); // V1 darkened
    const Sample inBlockBlue = sampleAt(sync, 15, 480, 270);
    CHECK(inBlockBlue.b > 200); // V2, above the lane, untouched
    CHECK(inBlockBlue.r < 30);
    CHECK(sampleAt(sync, 40, 1600, 900).r > 200); // after the block: red again
    CHECK(sampleAt(sync, 5, 1600, 900).r > 200);  // and before it

    // V1's clip keeps its own sound, at the same level, under the block.
    CHECK(before.rms > 1000.0);
    CHECK(inBlockRed.rms == doctest::Approx(before.rms).epsilon(0.01));

    // Lane 0 darkens everything, the picture above included.
    LaneScene all;
    all.addBlock(0);
    EngineSync everything(all.model);
    CHECK(everything.verify().empty());
    CHECK(sampleAt(everything, 15, 480, 270).b < 30);
}

TEST_CASE("FX4: a saved project plays its adjustment lanes in melt as the editor does")
{
    sharedFactoryPolicy();
    registerTestDropIn();
    LaneScene scene;
    scene.addBlock(1);
    const std::string project = (std::filesystem::temp_directory_path() /
                                 ("ustudio-lane-" + std::to_string(platform::currentProcessId()) + ".ustudio"))
                                    .string();
    REQUIRE(saveProject(scene.model, project).empty());
    auto loaded = loadProject(project);
    REQUIRE(loaded.has_value());
    CHECK(loaded->sequence().adjustmentBlocks == scene.model.sequence().adjustmentBlocks);

    EngineSync sync(scene.model);
    Mlt::Producer melt(sync.profile(), "xml", project.c_str());
    REQUIRE(melt.is_valid());
    for (int position : {5, 15, 40}) {
        INFO("frame " << position);
        auto grab = [position](Mlt::Producer &producer, std::unique_ptr<Mlt::Frame> &hold) {
            producer.seek(position);
            hold.reset(producer.get_frame());
            hold->set("consumer.rescale", "bilinear");
            mlt_image_format format = mlt_image_rgb;
            int w = 1920, h = 1080;
            return hold->get_image(format, w, h);
        };
        std::unique_ptr<Mlt::Frame> a, b;
        const uint8_t *editor = grab(sync.tractor(), a);
        const uint8_t *played = grab(melt, b);
        CHECK(std::memcmp(editor, played, size_t{1920} * 1080 * 3) == 0);
    }
    std::filesystem::remove(project);
}

// VE GPU's check of the FX4 nesting on the GPU pipeline (2026-09-28): the
// lane scenes above, rendered frame by frame through the GPU graph and the
// CPU one, with a third row for two nested lanes. Skips without GL.
namespace {

struct Frame1080
{
    std::vector<uint8_t> rgba;
    double ms = 0;
};

std::vector<Frame1080> renderAll(EngineSync &sync)
{
    std::vector<Frame1080> frames;
    for (int position = 0; position < 60; ++position) {
        const auto t0 = std::chrono::steady_clock::now();
        sync.tractor().seek(position);
        std::unique_ptr<Mlt::Frame> frame(sync.tractor().get_frame());
        frame->set("consumer.rescale", "bilinear");
        mlt_image_format format = mlt_image_rgba;
        int fw = 1920, fh = 1080;
        const uint8_t *image = frame->get_image(format, fw, fh);
        REQUIRE(image);
        Frame1080 f;
        f.rgba.assign(image, image + static_cast<size_t>(fw) * fh * 4);
        f.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        frames.push_back(std::move(f));
    }
    return frames;
}

double meanDiff(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b)
{
    double s = 0;
    for (size_t i = 0; i < a.size(); i += 4)
        s += std::abs(a[i] - b[i]) + std::abs(a[i + 1] - b[i + 1]) + std::abs(a[i + 2] - b[i + 2]);
    return s / static_cast<double>(a.size() / 4 * 3);
}

const uint8_t *at(const std::vector<uint8_t> &img, int x, int y)
{
    return img.data() + (static_cast<size_t>(y) * 1920 + static_cast<size_t>(x)) * 4;
}

void compareGpuToCpu(const Model &model, const char *what, double &inBlockMs, double &outsideMs)
{
    std::vector<Frame1080> cpu;
    {
        EngineSync sync(model);
        REQUIRE(sync.verify().empty());
        cpu = renderAll(sync);
    }
    std::string error;
    std::shared_ptr<GpuSession> session = GpuSession::acquire(error);
    REQUIRE(session);
    REQUIRE(session->renderThreadStarted());
    std::vector<Frame1080> gpu;
    {
        EngineSync sync(model);
        sync.setPipeline(EngineSync::Pipeline::Gpu, {});
        CHECK(sync.verify().empty());
        gpu = renderAll(sync);
    }
    session->renderThreadStopped();
    double worst = 0;
    int worstAt = -1;
    inBlockMs = outsideMs = 0;
    for (int p = 0; p < 60; ++p) {
        const double d = meanDiff(cpu[static_cast<size_t>(p)].rgba, gpu[static_cast<size_t>(p)].rgba);
        if (d > worst)
            worst = d, worstAt = p;
        (p >= 10 && p < 30 ? inBlockMs : outsideMs) += gpu[static_cast<size_t>(p)].ms;
    }
    inBlockMs /= 20;
    outsideMs /= 40;
    MESSAGE(std::string(what) << ": worst mean diff GPU vs CPU " << worst << " at frame " << worstAt
                              << "; GPU ms/frame in block " << inBlockMs << ", outside " << outsideMs);
    CHECK(worst <= 3.0);
    // The samples of the CPU test, on the GPU frames.
    const auto &in = gpu[15].rgba;
    CHECK(at(in, 1600, 900)[0] < 30);            // V1 darkened in the block
    CHECK(at(gpu[40].rgba, 1600, 900)[0] > 200); // red again after it
    CHECK(at(gpu[5].rgba, 1600, 900)[0] > 200);  // and before it
}

bool gpuHere()
{
    std::string error;
    return GpuSession::acquire(error) != nullptr;
}

} // namespace

TEST_CASE("FX4 on the GPU pipeline: a lane-1 block nests, filters and matches the CPU frame by frame")
{
    sharedFactoryPolicy();
    registerTestDropIn();
    if (!gpuHere()) {
        MESSAGE("no GPU pipeline here");
        return;
    }
    LaneScene scene;
    scene.addBlock(1);
    double inBlock = 0, outside = 0;
    compareGpuToCpu(scene.model, "lane 1", inBlock, outside); // V2, above the lane, matches the CPU's too
}

TEST_CASE("FX4 on the GPU pipeline: two nested lanes match the CPU frame by frame")
{
    sharedFactoryPolicy();
    registerTestDropIn();
    if (!gpuHere()) {
        MESSAGE("no GPU pipeline here");
        return;
    }
    LaneScene scene;
    // A third row on top: green in the bottom-right quarter.
    const TrackId third = scene.model.addTrack(Track::Kind::Video, 0, "V3");
    Asset green;
    green.path = "color:#00c000";
    green.info.hasVideo = true;
    green.info.lengthInSequenceFrames = 10'000;
    const ClipId picture = scene.model.insertClip(third, scene.model.addAsset(green), 0, 0, 59);
    Transform placed;
    placed.bounds = Transform::Bounds::None;
    placed.x.value = 1440;
    placed.y.value = 270;
    placed.width.value = 960;
    placed.height.value = 540;
    scene.model.setClipTransform(picture, placed);
    scene.addBlock(1); // rows 1-2 (V2, V1)
    scene.addBlock(2); // row 2 (V1), nested inside lane 1's
    double inBlock = 0, outside = 0;
    compareGpuToCpu(scene.model, "lanes 1+2", inBlock, outside);
}
