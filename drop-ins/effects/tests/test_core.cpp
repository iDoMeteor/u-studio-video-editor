// The effects drop-in's core/: JSON, descriptor normalisation and overlays,
// probe results, the commands' undo property, and core::nativeFilters()
// (the filters an effect becomes), without MLT.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/commands.h"
#include "core/commands/primitives.h"
#include "core/commands/undo_stack.h"
#include "core/descriptor.h"
#include "core/health.h"
#include "core/json.h"
#include "core/keyframes.h"
#include "core/looks.h"
#include "core/transitions.h"
#include "core/blocks.h"
#include "core/model/effect_native.h"
#include "core/model/animation.h"
#include "core/model/effect_native.h"
#include "core/model/transition_native.h"

#include <fstream>
#include <iterator>
#include <random>
#include <set>

using namespace ustudio;
using namespace ustudio::effects;

namespace {

std::string property(const core::NativeFilter &filter, const std::string &name)
{
    for (const auto &[key, value] : filter.properties)
        if (key == name)
            return value;
    return "<none>";
}

} // namespace

TEST_CASE("JSON: parses what we write and round-trips it")
{
    const std::string text =
        R"({"a":1.5,"b":[true,false,null],"c":"x\"y\\z\n\u00e9\ud83d\ude00","d":{"e":-2e3},"f":[]})";
    std::string error;
    std::optional<Json> json = parseJson(text, &error);
    REQUIRE_MESSAGE(json, error);
    CHECK((*json)["a"].asNumber() == 1.5);
    CHECK((*json)["b"].asArray().size() == 3);
    CHECK((*json)["b"].asArray()[2].isNull());
    CHECK((*json)["c"].asString() == "x\"y\\z\n\xc3\xa9\xf0\x9f\x98\x80");
    CHECK((*json)["d"]["e"].asNumber() == -2000.0);
    CHECK((*json)["missing"].isNull());
    CHECK(parseJson(toJson(*json)) == json);
}

TEST_CASE("JSON: malformed text is refused, not half-read")
{
    for (const char *bad :
         {"", "{", "{\"a\"}", "[1,]", "{\"a\":1,}", "tru", "\"unterminated", "01x", "[1] 2", "\"\\q\"", "\"a\x01\""}) {
        std::string error;
        CHECK_MESSAGE(!parseJson(bad, &error), bad);
        CHECK(!error.empty());
    }
    // Nesting far past anything we write is refused, not a stack overflow.
    CHECK(!parseJson(std::string(10'000, '[') + std::string(10'000, ']')));
}

TEST_CASE("Descriptors: every source type maps to one kind")
{
    auto kindOf = [](const char *family, RawParam raw) { return normaliseParam(family, raw).kind; };
    CHECK(kindOf("mlt", {.identifier = "level", .type = "float"}) == ParamKind::Scalar);
    CHECK(kindOf("mlt", {.identifier = "n", .type = "integer"}) == ParamKind::Integer);
    CHECK(kindOf("mlt", {.identifier = "on", .type = "integer", .widget = "checkbox"}) == ParamKind::Toggle);
    CHECK(kindOf("frei0r", {.identifier = "3", .type = "boolean", .widget = "checkbox"}) == ParamKind::Toggle);
    CHECK(kindOf("frei0r", {.identifier = "0", .type = "color", .widget = "color"}) == ParamKind::Color);
    CHECK(kindOf("mlt", {.identifier = "fg", .type = "string", .widget = "color"}) == ParamKind::Color);
    CHECK(kindOf("mlt", {.identifier = "geometry", .type = "rect"}) == ParamKind::Rect);
    CHECK(kindOf("mlt", {.identifier = "mode", .type = "string", .values = {"a", "b"}}) == ParamKind::Choice);
    CHECK(kindOf("mlt", {.identifier = "file", .type = "string", .widget = "fileopen"}) == ParamKind::File);
    CHECK(kindOf("frei0r", {.identifier = "0", .type = "string", .widget = "text"}) == ParamKind::Text);
}

TEST_CASE("Descriptors: plumbing is hidden, animation and defaults follow the metadata")
{
    CHECK(normaliseParam("mlt", {.identifier = "threads", .type = "integer"}).hidden);
    CHECK(normaliseParam("avfilter", {.identifier = "av.threads", .type = "integer"}).hidden);
    CHECK(normaliseParam("avfilter", {.identifier = "position", .type = "string"}).hidden);
    CHECK(normaliseParam("mlt", {.identifier = "producer.*", .type = "properties"}).hidden);
    CHECK(normaliseParam("mlt", {.identifier = "start", .title = "Start level (*DEPRECATED*)"}).hidden);
    CHECK(!normaliseParam("mlt", {.identifier = "level", .type = "float"}).hidden);

    // avfilter's numbers animate though its metadata doesn't say so.
    CHECK(normaliseParam("avfilter", {.identifier = "av.sigma", .type = "float"}).animatable);
    CHECK(!normaliseParam("mlt", {.identifier = "x", .type = "float"}).animatable);
    CHECK(normaliseParam("mlt", {.identifier = "x", .type = "float", .animation = true}).animatable);
    // Keyframes are numbers: a colour doesn't animate yet.
    CHECK(!normaliseParam("frei0r", {.identifier = "0", .type = "color", .animation = true}).animatable);

    ParamDescriptor withDefault =
        normaliseParam("frei0r", {.identifier = "0", .type = "float", .defaultValue = "0.25"});
    CHECK(withDefault.hasDefault);
    CHECK(std::get<double>(withDefault.defaultValue) == 0.25);
    CHECK(!normaliseParam("mlt", {.identifier = "level", .type = "float"}).hasDefault);
    // frei0r.defish0r's "Non-Linear scale" says its default is NaN.
    CHECK(!normaliseParam("frei0r", {.identifier = "9", .type = "float", .defaultValue = "nan"}).hasDefault);

    // MLT's colour forms.
    auto colour = [](const char *text) { return std::get<core::Color>(parseValue(ParamKind::Color, text)); };
    CHECK(colour("0xff000080") == core::Color{255, 0, 0, 128});
    CHECK(colour("#00ff00") == core::Color{0, 255, 0, 255});
    CHECK(colour("#8000ff00") == core::Color{0, 255, 0, 128}); // '#' with alpha: alpha first
}

TEST_CASE("Descriptors: family, media, overlays, and the cache's round trip")
{
    CHECK(familyOf("frei0r.glow") == "frei0r");
    CHECK(familyOf("avfilter.gblur") == "avfilter");
    CHECK(familyOf("brightness") == "mlt");
    CHECK(familyOf("some.thing") == "mlt");
    // Hardware-only FFmpeg filters are never offered (a crash in teardown
    // without the device); their software namesakes are.
    for (const char *hw : {"avfilter.blackdetect_vulkan", "avfilter.avgblur_opencl", "avfilter.scale_cuda",
                           "avfilter.denoise_vaapi", "avfilter.vpp_qsv", "avfilter.sr_amf", "avfilter.hwupload_cuda",
                           "avfilter.hwdownload", "avfilter.hwmap", "avfilter.libplacebo"})
        CHECK_MESSAGE(isHardwareOnlyFilter(hw), hw);
    for (const char *sw :
         {"avfilter.blackdetect", "avfilter.avgblur", "avfilter.gblur", "frei0r.glow", "brightness", "avfilter.vflip"})
        CHECK_MESSAGE(!isHardwareOnlyFilter(sw), sw);

    RawEffect raw{"frei0r.glow",
                  "Glow",
                  "Creates a Glamorous Glow",
                  {"Video"},
                  {{.identifier = "0",
                    .title = "Blur",
                    .type = "float",
                    .widget = "spinner",
                    .minimum = 0,
                    .maximum = 1,
                    .defaultValue = "0",
                    .animation = true}}};
    EffectDescriptor glow = normalise(raw);
    CHECK(glow.family == "frei0r");
    CHECK(glow.media == MediaKind::Video);
    CHECK(normalise({"volume", "Volume", "", {"Audio"}, {}}).media == MediaKind::Audio);

    std::optional<Json> overlay = parseJson(R"({"name":"Glow","category":"Light","featured":true,
        "params":{"0":{"name":"Amount","default":0.5,"display":{"from":[0,1],"to":[0,100],"unit":"%"}},
                  "missing":{"name":"ignored"}}})");
    REQUIRE(overlay);
    applyOverlay(glow, *overlay);
    CHECK(glow.category == "Light");
    CHECK(glow.featured);
    REQUIRE(glow.params.size() == 1);
    CHECK(glow.params[0].title == "Amount");
    CHECK(std::get<double>(glow.params[0].defaultValue) == 0.5);
    REQUIRE(glow.params[0].display);
    CHECK(glow.params[0].display->toDisplay(0.25) == 25.0);
    CHECK(glow.params[0].display->fromDisplay(50.0) == 0.5);

    // A malformed overlay changes nothing.
    EffectDescriptor before = glow;
    applyOverlay(glow, Json("not an object"));
    applyOverlay(glow, *parseJson(R"({"params":{"0":{"display":{"from":[0]}}}})"));
    CHECK(glow == before);

    std::optional<EffectDescriptor> back = descriptorFromJson(*parseJson(toJson(toJson(glow))));
    REQUIRE(back);
    CHECK(*back == glow);

    core::Effect effect = makeEffect(glow);
    CHECK(effect.service == "frei0r.glow");
    CHECK(effect.owner == "effects");
    CHECK(effect.displayName == "Glow");
    REQUIRE(effect.params.size() == 1);
    CHECK(std::get<double>(effect.params[0].value) == 0.5);
}

TEST_CASE("Health: the probe's line and the health file round-trip; badges by cost")
{
    const HealthRecord bad{HealthStatus::BadOutput, "black picture with its defaults", 3.5};
    std::optional<std::pair<std::string, HealthRecord>> parsed = parseProbeResultLine(probeResultLine("frei0r.x", bad));
    REQUIRE(parsed);
    CHECK(parsed->first == "frei0r.x");
    CHECK(parsed->second == bad);
    CHECK(!parseProbeResultLine("[mlt] some log line"));
    CHECK(!parseProbeResultLine(R"({"service":"a","status":"exploded"})"));

    HealthFile file;
    file.fingerprint = "abc";
    file.records["frei0r.x"] = bad;
    file.records["frei0r.y"] = {HealthStatus::Ok, "", 1.0};
    const HealthFile back = healthFileFromJson(*parseJson(toJson(toJson(file))));
    CHECK(back.fingerprint == "abc");
    CHECK(back.records == file.records);
    CHECK(back.quarantined("frei0r.x"));
    CHECK(!back.quarantined("frei0r.y"));
    CHECK(!back.quarantined("frei0r.never-probed"));

    // A child's output: its result, or where it died.
    const std::string stages =
        probeStageLine("frei0r.x", "defaults") + "\n" + probeStageLine("frei0r.x", "maximums") + "\n";
    CHECK(interpretProbe("frei0r.x", stages + probeResultLine("frei0r.x", bad) + "\n", false) == bad);
    const HealthRecord died = interpretProbe("frei0r.x", stages, false);
    CHECK(died.status == HealthStatus::Crashed);
    CHECK(died.reason == "the probe died (maximums)");
    CHECK(interpretProbe("frei0r.x", stages, true).status == HealthStatus::TimedOut);
    CHECK(interpretProbe("frei0r.x", "", false).reason == "the probe died");
    // Another service's line doesn't count.
    CHECK(interpretProbe("frei0r.x", probeResultLine("frei0r.y", {}) + "\n", false).status == HealthStatus::Crashed);

    CHECK(costBadge(2.0) == CostBadge::Light);
    CHECK(costBadge(12.0) == CostBadge::Medium);
    CHECK(costBadge(40.0) == CostBadge::Heavy);
}

TEST_CASE("nativeFilters: a full mix is the effect alone; any other wraps it in mask_start / mask_apply")
{
    core::Effect effect;
    effect.service = "frei0r.glow";
    core::Param blur;
    blur.name = "0";
    blur.value = 0.5;
    blur.keyframes = {{0, 0.0, core::Easing::Linear}, {100, 1.0, core::Easing::CubicOut}};
    effect.params = {blur};

    std::vector<core::NativeFilter> plain = core::nativeFilters(effect, 50, 50);
    REQUIRE(plain.size() == 1);
    CHECK(plain[0].service == "frei0r.glow");
    // Shifted to the cut: the keyframe at 0 falls before it, so the cut
    // starts on the value there.
    CHECK(property(plain[0], "0").starts_with("0="));
    CHECK(property(plain[0], "disable") == "<none>");

    effect.mix = {0.5, {}};
    effect.enabled = false;
    std::vector<core::NativeFilter> wrapped = core::nativeFilters(effect, 0, 100);
    REQUIRE(wrapped.size() == 2);
    CHECK(wrapped[0].service == "mask_start");
    CHECK(property(wrapped[0], "filter") == "frei0r.glow");
    CHECK(property(wrapped[0], "filter.0").starts_with("0=0;")); // the effect's own parameters, prefixed
    CHECK(wrapped[1].service == "mask_apply");
    // Never mask_apply's default (qtblend: ADR-007).
    CHECK(property(wrapped[1], "transition") == "frei0r.cairoblend");
    CHECK(property(wrapped[1], "transition.0") == "0.5");
    CHECK(property(wrapped[0], "disable") == "1");
    CHECK(property(wrapped[1], "disable") == "1");

    effect.enabled = true;
    std::vector<core::NativeFilter> affine = core::nativeFilters(effect, 0, 100, core::MixTransition::Affine);
    REQUIRE(affine.size() == 2);
    CHECK(property(affine[1], "transition") == "affine");
    CHECK(property(affine[1], "transition.rect") == "0% 0% 100% 100% 50%");

    // A keyframed mix animates brightness's alpha between the pair; the
    // transition stays opaque (a transition's animation reads the wrong
    // position inside a playlist).
    effect.mix = {1.0, {{0, 0.0, core::Easing::Linear}, {10, 1.0, core::Easing::Linear}}};
    std::vector<core::NativeFilter> animated = core::nativeFilters(effect, 0, 100);
    REQUIRE(animated.size() == 3);
    CHECK(animated[1].service == "brightness");
    CHECK(property(animated[1], "level") == "1");
    CHECK(property(animated[1], "alpha") == "0=0;10=1");
    CHECK(property(animated[2], "transition.0") == "1");
}

namespace {

// Equal but for nextId, which never rolls back (tests/core/test_commands.cpp).
bool sameProject(core::Project a, core::Project b)
{
    a.nextId = b.nextId = 0;
    return a == b;
}

std::vector<core::EffectId> allEffects(const core::Model &model)
{
    std::vector<core::EffectId> ids;
    auto collect = [&](const std::vector<core::Effect> &effects) {
        for (const core::Effect &effect : effects)
            ids.push_back(effect.id);
    };
    collect(model.sequence().effects);
    for (const core::Track &track : model.sequence().tracks) {
        collect(track.effects);
        for (core::ClipId clip : track.clips)
            collect(model.clip(clip).effects);
    }
    return ids;
}

core::Param randomParam(std::mt19937 &rng)
{
    core::Param param;
    param.name = std::to_string(rng() % 3);
    param.value = static_cast<double>(rng() % 100) / 100.0;
    core::FrameIndex at = 0;
    for (int k = 0; k < static_cast<int>(rng() % 3); ++k) {
        at += 1 + static_cast<core::FrameIndex>(rng() % 20);
        param.keyframes.push_back(
            {at, static_cast<double>(rng() % 100) / 100.0, static_cast<core::Easing>(rng() % 35)});
    }
    return param;
}

} // namespace

TEST_CASE("Commands: random effect edits, undo all and redo all, equal; gestures merge")
{
    for (uint32_t seed : {1u, 2u, 3u, 4u}) {
        core::Model model = core::Model::createEmpty();
        core::TrackId track = model.addTrack(core::Track::Kind::Video, 0, "V1");
        core::Asset asset;
        asset.path = "color:red";
        asset.info.hasVideo = true;
        asset.info.lengthInSequenceFrames = 100'000;
        core::AssetId assetId = model.addAsset(asset);
        for (int i = 0; i < 4; ++i)
            model.insertClip(track, assetId, i * 100, 0, 99);
        core::UndoStack undo(model);
        undo.limit = 100'000;
        const core::Project start = model.project();
        std::mt19937 rng(seed);
        uint64_t gesture = 1;

        for (int i = 0; i < 1000; ++i) {
            const std::vector<core::EffectId> effects = allEffects(model);
            const auto &clips = model.track(track).clips;
            auto pickEffect = [&] { return effects[rng() % effects.size()]; };
            std::unique_ptr<core::Command> command;
            switch (rng() % 8) {
            case 0:
            case 1: {
                Target target = Target::sequence();
                if (rng() % 3)
                    target = Target::clip(clips[rng() % clips.size()]);
                else if (rng() % 2)
                    target = Target::track(track);
                core::Effect effect;
                effect.service = "frei0r.glow";
                effect.owner = kOwner;
                if (rng() % 2)
                    effect.params = {randomParam(rng)};
                command = std::make_unique<AddEffect>(target, effect, rng() % 3);
                break;
            }
            case 2:
                if (!effects.empty())
                    command = std::make_unique<RemoveEffect>(pickEffect());
                break;
            case 3:
                if (!effects.empty())
                    command = std::make_unique<MoveEffect>(pickEffect(), rng() % 3);
                break;
            case 4:
                if (!effects.empty())
                    command = std::make_unique<SetEffectEnabled>(pickEffect(), rng() % 2);
                break;
            case 5:
            case 6:
                if (!effects.empty())
                    command =
                        std::make_unique<SetParam>(pickEffect(), randomParam(rng), rng() % 2 ? gesture : ++gesture);
                break;
            case 7:
                if (!effects.empty()) {
                    core::KeyframedValue mix{static_cast<double>(rng() % 101) / 100.0, {}};
                    command = std::make_unique<SetMix>(pickEffect(), mix, rng() % 2 ? gesture : ++gesture);
                }
                break;
            }
            if (command)
                undo.execute(std::move(command));
            REQUIRE(model.check().empty());
            REQUIRE(model.snapshot()->operator==(model.project()));
        }
        const core::Project end = model.project();
        while (undo.undo())
            REQUIRE(model.check().empty());
        CHECK(sameProject(model.project(), start));
        while (undo.redo())
            REQUIRE(model.check().empty());
        CHECK(sameProject(model.project(), end));
    }
}

TEST_CASE("Commands: refusals change nothing")
{
    core::Model model = core::Model::createEmpty();
    core::TrackId track = model.addTrack(core::Track::Kind::Video, 0, "V1");
    core::Effect effect;
    effect.service = "brightness";
    effect.owner = kOwner;
    core::EffectId id = model.addEffect(Target::track(track), effect, 0);
    const core::Project before = model.project();

    core::Param unsorted;
    unsorted.name = "level";
    unsorted.value = 1.0;
    unsorted.keyframes = {{10, 0.0, core::Easing::Linear}, {5, 1.0, core::Easing::Linear}};
    SetParam badKeys(id, unsorted);
    CHECK(!badKeys.apply(model));
    core::Param textKeys;
    textKeys.name = "t";
    textKeys.value = std::string("x");
    textKeys.keyframes = {{0, 0.0, core::Easing::Linear}};
    CHECK(!SetParam(id, textKeys).apply(model));
    CHECK(!SetMix(id, {1.5, {}}).apply(model));
    CHECK(!SetMix(id, {0.5, {{0, 2.0, core::Easing::Linear}}}).apply(model));
    CHECK(!AddEffect(Target::clip(core::ClipId{999}), effect, 0).apply(model));
    CHECK(!RemoveEffect(core::EffectId{999}).apply(model));
    CHECK(!SetEffectEnabled(id, true).apply(model)); // already on
    CHECK(model.project() == before);

    // A gesture that ends where it began is a no-op, which the stack drops.
    core::UndoStack undo(model);
    core::Param level;
    level.name = "level";
    level.value = 1.0;
    undo.execute(std::make_unique<SetParam>(id, level, 7)); // adds the parameter
    level.value = 0.4;
    undo.execute(std::make_unique<SetParam>(id, level, 8));
    level.value = 0.9;
    undo.execute(std::make_unique<SetParam>(id, level, 8)); // merges into the previous
    CHECK(std::get<double>(model.effect(id).params[0].value) == 0.9);
    undo.undo();
    CHECK(std::get<double>(model.effect(id).params[0].value) == 1.0);
    undo.undo(); // the parameter goes again: the effect as it was
    CHECK(sameProject(model.project(), before));
}

TEST_CASE("Keyframes: pin, move between, re-ease, and the feels")
{
    std::vector<core::Keyframe> keys;
    keys = withKeyAt(keys, 30, 1.0);
    keys = withKeyAt(keys, 0, 0.0);
    keys = withKeyAt(keys, 15, 0.25, core::Easing::CubicIn);
    REQUIRE(keys.size() == 3);
    CHECK(keys[0].at == 0);
    CHECK(keys[1].at == 15);
    CHECK(keys[2].at == 30);         // kept in order
    keys = withKeyAt(keys, 15, 0.5); // an existing key: its value only
    CHECK(keys.size() == 3);
    CHECK(keys[1].value == 0.5);
    CHECK(keys[1].easing == core::Easing::CubicIn);
    CHECK(keyAt(keys, 15));
    CHECK(!keyAt(keys, 16));
    CHECK(previousKey(keys, 15) == 0);
    CHECK(previousKey(keys, 0) == std::nullopt);
    CHECK(nextKey(keys, 15) == 30);
    CHECK(nextKey(keys, 16) == 30);
    CHECK(nextKey(keys, 30) == std::nullopt);
    keys = withEasingAt(keys, 0, core::Easing::BounceOut);
    CHECK(keys[0].easing == core::Easing::BounceOut);
    keys = withoutKeyAt(keys, 15);
    CHECK(keys.size() == 2);
    CHECK(withoutKeyAt(keys, 99) == keys);
    // Halfway between 0 (0.0) and 30 (1.0), linear: 0.5.
    CHECK(core::easedValue(withEasingAt(keys, 0, core::Easing::Linear), 15) == doctest::Approx(0.5));

    REQUIRE(feels().size() == 8);
    CHECK(feelName(core::Easing::Discrete) == "Hold");
    CHECK(feelName(core::Easing::SmoothNatural) == "Smooth");
    CHECK(feelName(core::Easing::QuarticIn) == core::easingName(core::Easing::QuarticIn));
}

TEST_CASE("Paste, looks and several clips at once: one undo step each, exact")
{
    core::Model model = core::Model::createEmpty();
    core::TrackId track = model.addTrack(core::Track::Kind::Video, 0, "V1");
    core::Asset asset;
    asset.path = "color:red";
    asset.info.hasVideo = true;
    asset.info.lengthInSequenceFrames = 1000;
    core::AssetId assetId = model.addAsset(asset);
    core::ClipId a = model.insertClip(track, assetId, 0, 0, 99);
    core::ClipId b = model.insertClip(track, assetId, 100, 0, 99);
    core::Effect glow;
    glow.service = "frei0r.glow";
    glow.owner = kOwner;
    core::Effect foreign;
    foreign.service = "brightness";
    foreign.owner = "someone-else";
    model.addEffect(Target::clip(b), glow, 0);
    model.addEffect(Target::clip(b), foreign, 1);
    core::UndoStack undo(model);
    const core::Project start = model.project();

    core::Effect sepia;
    sepia.service = "sepia";
    sepia.owner = kOwner;
    // Append onto both: a gets sepia, b gets it after its two.
    REQUIRE(undo.execute(
        pasteEffects(model, {Target::clip(a), Target::clip(b)}, {sepia}, PasteMode::Append, "Paste effects")));
    CHECK(model.clip(a).effects.size() == 1);
    CHECK(model.clip(b).effects.size() == 3);
    CHECK(model.clip(b).effects[2].service == "sepia");
    // Replace: b loses its own glow and sepia, keeps the other drop-in's.
    REQUIRE(undo.execute(pasteEffects(model, {Target::clip(b)}, {sepia, glow}, PasteMode::Replace, "Paste effects")));
    REQUIRE(model.clip(b).effects.size() == 3);
    CHECK(model.clip(b).effects[0].owner == "someone-else");
    CHECK(model.clip(b).effects[1].service == "sepia");
    CHECK(model.clip(b).effects[2].service == "frei0r.glow");
    REQUIRE(model.check().empty());

    // Several clips: one parameter on both sepias, one undo step per gesture.
    const std::vector<core::EffectId> sepias = {model.clip(a).effects[0].id, model.clip(b).effects[1].id};
    core::Param level;
    level.name = "u";
    level.value = 0.2;
    undo.execute(setParamOnAll(sepias, level, 7));
    level.value = 0.4;
    undo.execute(setParamOnAll(sepias, level, 7)); // the same drag
    undo.execute(setMixOnAll(model, sepias, 0.5, 8));
    CHECK(std::get<double>(model.effect(sepias[0]).params[0].value) == 0.4);
    CHECK(model.effect(sepias[1]).mix.value == 0.5);

    // Looks: saved, deleted from the middle, undone back in place.
    core::Look first{{}, "First", {sepia}}, second{{}, "Second", {glow}}, third{{}, "Third", {sepia, glow}};
    for (const core::Look &look : {first, second, third})
        REQUIRE(undo.execute(std::make_unique<SaveLook>(look)));
    const core::Project withLooks = model.project();
    REQUIRE(undo.execute(std::make_unique<DeleteLook>(withLooks.looks[1].id)));
    CHECK(model.project().looks.size() == 2);
    undo.undo();
    CHECK(sameProject(model.project(), withLooks)); // the order too
    CHECK(!SaveLook(core::Look{{}, "", {sepia}}).apply(model));

    while (undo.canUndo())
        undo.undo();
    CHECK(sameProject(model.project(), start));
}

TEST_CASE("Brand looks: the shipped file parses; bad entries are skipped")
{
    std::optional<Json> json = parseJson(R"({"version":1,"looks":[
        {"name":"Good","effects":[{"service":"frei0r.glow","params":{"0":0.3,"x":true,"t":"s"},"mix":0.8}]},
        {"name":"No service","effects":[{"params":{}}]},
        {"effects":[{"service":"sepia"}]},
        {"name":"Empty","effects":[]}]})");
    REQUIRE(json);
    const std::vector<core::Look> looks = looksFromJson(*json);
    REQUIRE(looks.size() == 1);
    CHECK(looks[0].name == "Good");
    REQUIRE(looks[0].effects.size() == 1);
    CHECK(looks[0].effects[0].owner == kOwner);
    CHECK(looks[0].effects[0].mix.value == 0.8);
    CHECK(looks[0].effects[0].params.size() == 3);
    CHECK(looksFromJson(Json(Json::Object{})).empty());

    std::ifstream in(EFFECTS_DATA_SOURCE_DIR "/looks/brand.json");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::optional<Json> brand = parseJson(text);
    REQUIRE(brand);
    CHECK(looksFromJson(*brand).size() == 5);
}

// --- Transition recipes (FX3) ------------------------------------------------

namespace {

// Two clips joined by a dissolve, and its id.
std::pair<core::Model, core::TransitionId> modelWithDissolve()
{
    core::Model model = core::Model::createEmpty();
    core::TrackId track = model.addTrack(core::Track::Kind::Video, 0, "V1");
    core::Asset asset;
    asset.path = "color:red";
    asset.info.hasVideo = true;
    asset.info.lengthInSequenceFrames = 100'000;
    core::AssetId id = model.addAsset(asset);
    core::ClipId a = model.insertClip(track, id, 0, 0, 49);
    core::ClipId b = model.insertClip(track, id, 50, 10, 59);
    core::TransitionId t = model.addTransition(track, a, b, 5, 5);
    return {std::move(model), t};
}

} // namespace

TEST_CASE("Transition recipes: the shipped set loads, 20 or more, the plain dissolve first")
{
    const std::vector<TransitionRecipe> recipes = loadRecipes(EFFECTS_DATA_SOURCE_DIR "/transitions");
    REQUIRE(recipes.size() >= 20);
    CHECK(recipes.front().id == kDefaultRecipe);
    CHECK(recipes.front().params.empty());
    std::set<std::string> ids;
    for (const TransitionRecipe &recipe : recipes) {
        CHECK(ids.insert(recipe.id).second);
        CHECK_FALSE(recipe.name.empty());
        CHECK_FALSE(recipe.category.empty());
        // Every wipe names a map the generator makes.
        for (const core::Param &param : recipe.params)
            if (param.name == "video.luma") {
                const auto &names = core::lumaMapNames();
                CHECK(std::find(names.begin(), names.end(), std::get<std::string>(param.value)) != names.end());
            }
    }
}

TEST_CASE("Transition recipes: a recipe naming a service outside the allowlist is skipped")
{
    std::optional<Json> json = parseJson(R"({"version":1,"recipes":[
        {"id":"ok","name":"Ok","params":{"video.softness":0.2},"exposed":["video.softness","video.missing"]},
        {"id":"evil","name":"Evil","params":{"video.service":"qtblend"}},
        {"id":"file","name":"File","params":{"video.resource":"/etc/passwd"}},
        {"name":"No id"}]})");
    REQUIRE(json);
    const std::vector<TransitionRecipe> recipes = recipesFromJson(*json);
    REQUIRE(recipes.size() == 1);
    CHECK(recipes[0].id == "ok");
    CHECK(recipes[0].exposed == std::vector<std::string>{"video.softness"});
}

TEST_CASE("SetTransitionRecipe: one undo step, refused on a locked track or a hostile service")
{
    auto [model, id] = modelWithDissolve();
    const core::Model before = model;
    core::UndoStack stack(model);
    const std::vector<core::Param> wipe{{"video.luma", std::string("radial"), {}}, {"video.softness", 0.1, {}}};
    REQUIRE(stack.execute(std::make_unique<SetTransitionRecipe>(id, "wipe.radial", wipe)));
    CHECK(model.transition(id).recipe == "wipe.radial");
    CHECK(model.transition(id).params == wipe);
    stack.undo();
    CHECK(model == before);
    stack.redo();
    CHECK(model.transition(id).recipe == "wipe.radial");

    // A softness drag is one step, and one that ends where it began is none.
    for (double softness : {0.2, 0.3, 0.4})
        stack.execute(std::make_unique<SetTransitionRecipe>(
            id, "wipe.radial", withParam(wipe, {"video.softness", softness, {}}), 7));
    stack.undo();
    CHECK(model.transition(id).params == wipe);
    stack.undo();
    CHECK(model == before);
    stack.redo();
    const core::UndoStack::State wiped = stack.state();
    for (double softness : {0.5, 0.1})
        stack.execute(std::make_unique<SetTransitionRecipe>(
            id, "wipe.radial", withParam(wipe, {"video.softness", softness, {}}), 8));
    CHECK(stack.state() == wiped);

    CHECK_FALSE(stack.execute(
        std::make_unique<SetTransitionRecipe>(id, "evil", std::vector<core::Param>{{"video.service", std::string("qtblend"), {}}})));
    CHECK(model.transition(id).recipe == "wipe.radial");
    const core::Transition &t = model.transition(id);
    model.setTrackFlags(t.track, false, false, true);
    CHECK_FALSE(stack.execute(std::make_unique<SetTransitionRecipe>(id, kDefaultRecipe, std::vector<core::Param>{})));
}

TEST_CASE("recipeIndexOf: a transition's recipe, the dissolve for none or an unknown one")
{
    std::vector<TransitionRecipe> recipes(2);
    recipes[0].id = kDefaultRecipe;
    recipes[1].id = "wipe.left";
    core::Transition t;
    CHECK(recipeIndexOf(recipes, t) == 0);
    t.recipe = "wipe.left";
    CHECK(recipeIndexOf(recipes, t) == 1);
    t.recipe = "from.a.newer.version";
    CHECK(recipeIndexOf(recipes, t) == 0);
    CHECK(recipeIndexOf({}, t) == -1);
}

// --- Touch-record (FX4) --------------------------------------------------------

TEST_CASE("withRecording: a performed curve becomes far fewer keys that stay within the tolerance")
{
    // Two seconds at 30 fps of a smooth performance (a sine sweep) with a
    // hold in the middle, as a hand on a slider would make it.
    std::vector<std::pair<core::FrameIndex, double>> performed;
    for (core::FrameIndex f = 10; f < 70; ++f) {
        const double v = f < 30 ? std::sin(static_cast<double>(f - 10) / 20.0 * 1.5707963) : (f < 45 ? 1.0 : 1.0 - static_cast<double>(f - 45) / 25.0);
        performed.emplace_back(f, v);
    }
    const std::vector<core::Keyframe> before{{0, 0.5, core::Easing::Linear}, {40, 9.0, core::Easing::Linear},
                                             {100, 0.2, core::Easing::Linear}};
    const double tolerance = recordingTolerance(0.0, 1.0);
    const std::vector<core::Keyframe> keys = withRecording(before, performed, tolerance);
    // The key inside the recorded range is replaced; those outside stay.
    CHECK(keys.front() == before.front());
    CHECK(keys.back() == before.back());
    CHECK_FALSE(std::any_of(keys.begin(), keys.end(), [](const core::Keyframe &k) { return k.value == 9.0; }));
    // The ends of the recording are kept.
    CHECK(keyAt(keys, 10));
    CHECK(keyAt(keys, 69));
    // Fewer keys than frames: an editable curve.
    const size_t recorded = keys.size() - 2;
    MESSAGE(recorded << " keys for " << performed.size() << " frames");
    CHECK(recorded < performed.size() / 3);
    // And it still plays what was performed, within the tolerance.
    for (const auto &[frame, value] : performed)
        CHECK(std::abs(core::easedValue(keys, static_cast<double>(frame)) - value) <= tolerance + 1e-9);
}

TEST_CASE("withRecording: repeated frames keep the last value; an empty recording changes nothing")
{
    const std::vector<core::Keyframe> keys = withRecording({}, {{5, 0.1}, {5, 0.4}, {6, 0.5}}, 0.001);
    REQUIRE(keys.size() == 2);
    CHECK(keys[0].value == doctest::Approx(0.4));
    const std::vector<core::Keyframe> untouched{{3, 1.0, core::Easing::SmoothNatural}};
    CHECK(withRecording(untouched, {}, 0.01) == untouched);
}

// --- Adjustment blocks (FX4) ----------------------------------------------------

namespace {

core::Model modelWithTracks(int tracks)
{
    core::Model model = core::Model::createEmpty();
    for (int i = 0; i < tracks; ++i)
        model.addTrack(core::Track::Kind::Video, i, "V" + std::to_string(i + 1));
    return model;
}

// Undo restores ids, but a restore still advances the id allocator
// (Model::add*'s reuseId; the core tests' equalIgnoringIdAllocator()).
bool sameIgnoringIdAllocator(const core::Model &a, const core::Model &b)
{
    core::Project x = a.project(), y = b.project();
    x.nextId = y.nextId = 0;
    return x == y;
}

core::Effect glowEffect(double mix = 1.0)
{
    core::Effect effect;
    effect.service = "frei0r.glow";
    effect.owner = kOwner;
    effect.mix.value = mix;
    return effect;
}

} // namespace

TEST_CASE("Adjustment blocks: add, move, fade and remove, each one undo step; overlaps refused")
{
    core::Model model = modelWithTracks(2);
    const core::Model empty = model;
    core::UndoStack stack(model);
    core::AdjustmentBlock block;
    block.lane = 0;
    block.start = 30;
    block.length = 60;
    block.effects = {glowEffect()};
    auto add = std::make_unique<AddAdjustmentBlock>(block);
    AddAdjustmentBlock *adding = add.get();
    REQUIRE(stack.execute(std::move(add)));
    const core::AdjustmentBlockId id = adding->id();
    REQUIRE(model.hasAdjustmentBlock(id));
    CHECK(model.adjustmentBlock(id).effects[0].id.isValid());
    CHECK(model.check().empty());

    // Overlapping on the same lane: refused; on another lane: fine.
    core::AdjustmentBlock overlap = block;
    overlap.start = 60;
    CHECK_FALSE(stack.execute(std::make_unique<AddAdjustmentBlock>(overlap)));
    overlap.lane = 1;
    CHECK(stack.execute(std::make_unique<AddAdjustmentBlock>(overlap)));
    stack.undo();
    // Past the tracks: refused.
    overlap.lane = 2;
    CHECK_FALSE(stack.execute(std::make_unique<AddAdjustmentBlock>(overlap)));

    // A drag: many moves, one step; fades shrink with the block.
    const core::Model placed = model;
    CHECK(stack.execute(std::make_unique<SetAdjustmentBlockFades>(id, core::FadeSpec{20}, core::FadeSpec{20})));
    for (core::FrameIndex length : {50, 40, 10})
        stack.execute(std::make_unique<SetAdjustmentBlockRange>(id, 0, 35, length, 9));
    CHECK(model.adjustmentBlock(id).start == 35);
    CHECK(model.adjustmentBlock(id).fadeIn->length == 10);
    stack.undo();
    CHECK(model.adjustmentBlock(id).length == 60);
    CHECK(model.adjustmentBlock(id).fadeIn->length == 20);
    stack.undo();
    CHECK(sameIgnoringIdAllocator(model, placed));
    CHECK_FALSE(stack.execute(std::make_unique<SetAdjustmentBlockFades>(id, core::FadeSpec{61}, std::nullopt)));

    // Remove, undo: the same block, ids and all.
    REQUIRE(stack.execute(std::make_unique<RemoveAdjustmentBlock>(id)));
    CHECK_FALSE(model.hasAdjustmentBlock(id));
    stack.undo();
    CHECK(sameIgnoringIdAllocator(model, placed));
    stack.undo();
    CHECK(sameIgnoringIdAllocator(model, empty));
    stack.redo();
    CHECK(sameIgnoringIdAllocator(model, placed));
}

TEST_CASE("blockEffects: fades ramp each effect's mix in and out")
{
    core::AdjustmentBlock block;
    block.length = 101;
    block.effects = {glowEffect(0.8)};
    CHECK(core::blockEffects(block)[0].mix.keyframes.empty()); // no fades: as it is

    block.fadeIn = core::FadeSpec{20};
    block.fadeOut = core::FadeSpec{40};
    const std::vector<core::Keyframe> keys = core::blockEffects(block)[0].mix.keyframes;
    auto at = [&](double frame) { return core::easedValue(keys, frame); };
    CHECK(at(0) == doctest::Approx(0.0));
    CHECK(at(10) == doctest::Approx(0.4));
    CHECK(at(20) == doctest::Approx(0.8));
    CHECK(at(50) == doctest::Approx(0.8));
    CHECK(at(60) == doctest::Approx(0.8));
    CHECK(at(80) == doctest::Approx(0.4));
    CHECK(at(100) == doctest::Approx(0.0));

    // An animated mix keeps its shape under the envelope.
    block.effects[0].mix.keyframes = {{0, 0.5, core::Easing::Linear}, {100, 1.0, core::Easing::Linear}};
    const std::vector<core::Keyframe> shaped = core::blockEffects(block)[0].mix.keyframes;
    CHECK(core::easedValue(shaped, 50.0) == doctest::Approx(0.75));
    CHECK(core::easedValue(shaped, 100.0) == doctest::Approx(0.0));
}

TEST_CASE("blockEffects: an eased mix keeps its easing between the fades, and follows it through them")
{
    core::AdjustmentBlock block;
    block.length = 201;
    block.fadeIn = core::FadeSpec{20};
    block.fadeOut = core::FadeSpec{20};
    block.effects = {glowEffect()};
    // Ease in from 0.2 to 1 across the whole block: a fade boundary cuts
    // the one segment at each end.
    const std::vector<core::Keyframe> own{{0, 0.2, core::Easing::CubicInOut}, {200, 1.0, core::Easing::Linear}};
    block.effects[0].mix.keyframes = own;
    const std::vector<core::Keyframe> keys = core::blockEffects(block)[0].mix.keyframes;
    auto envelope = [](double f) { return std::clamp(std::min(f / 20.0, (200.0 - f) / 20.0), 0.0, 1.0); };
    for (double f : {0.0, 5.0, 10.0, 20.0, 50.0, 100.0, 137.0, 180.0, 190.0, 200.0})
        CHECK(core::easedValue(keys, f) == doctest::Approx(core::easedValue(own, f) * envelope(f)).epsilon(0.002));

    // A segment wholly between the fades keeps its own key and easing.
    const std::vector<core::Keyframe> inner{{40, 0.2, core::Easing::CubicIn}, {160, 1.0, core::Easing::Linear}};
    block.effects[0].mix.keyframes = inner;
    const std::vector<core::Keyframe> kept = core::blockEffects(block)[0].mix.keyframes;
    auto at40 = std::find_if(kept.begin(), kept.end(), [](const core::Keyframe &k) { return k.at == 40; });
    REQUIRE(at40 != kept.end());
    CHECK(at40->easing == core::Easing::CubicIn);
    CHECK(core::easedValue(kept, 100.0) == doctest::Approx(core::easedValue(inner, 100.0)));
    CHECK(kept.size() < 60); // only the fades are sampled
}

TEST_CASE("Overlays: a parameter's kind and file extensions, kept through the registry cache")
{
    EffectDescriptor d;
    d.service = "avfilter.lut3d";
    ParamDescriptor file;
    file.id = "av.file";
    file.kind = ParamKind::Text;
    d.params = {file};
    std::optional<Json> overlay = parseJson(R"j({"name":"LUT (.cube)","params":{"av.file":{"kind":"file","extensions":["cube"]}}})j");
    REQUIRE(overlay);
    applyOverlay(d, *overlay);
    CHECK(d.params[0].kind == ParamKind::File);
    CHECK(d.params[0].extensions == std::vector<std::string>{"cube"});
    const std::optional<EffectDescriptor> back = descriptorFromJson(toJson(d));
    REQUIRE(back);
    CHECK(*back == d);
    // An unknown kind changes nothing.
    std::optional<Json> odd = parseJson(R"({"params":{"av.file":{"kind":"spaceship"}}})");
    applyOverlay(d, *odd);
    CHECK(d.params[0].kind == ParamKind::File);
}
