// IP3 (doc 15): EngineSync with the test drop-in's EngineExtension, built in
// and as a module. Every hook runs where doc 15 says, effects play animated
// per cut (dissolve tails and heads included), a value change is applied in
// place without a rebuild, and with no extension nothing changes.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/model/model.h"
#include "dropins/registry.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"
#include "engine/test_extension.h"

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

// color:red's red channel through a timeline graph, measured.
constexpr int kFullRed = 233;

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
