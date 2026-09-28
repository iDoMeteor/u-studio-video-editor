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
#include "core/model/animation.h"
#include "core/model/effect_native.h"

#include <random>

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
