// IP1's property test (doc 15, "Not purely additive"): random effect
// commands (the test drop-in's, on Model's IP1 mutators) mixed with clip
// inserts, splits and removals; after every step the model checks clean and
// its snapshot equals the project; undo all returns the start exactly and
// redo all the end.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/commands/primitives.h"
#include "core/commands/undo_stack.h"
#include "core/model/model.h"
#include "effect_commands.h"

#include <random>

using namespace ustudio;
using namespace ustudio::core;
using namespace ustudio::testdropin;

namespace {

Param animated(std::mt19937 &rng, const std::string &name)
{
    Param param;
    param.name = name;
    param.value = static_cast<double>(rng() % 100) / 100.0;
    FrameIndex at = -static_cast<FrameIndex>(rng() % 5);
    for (int k = 0; k < static_cast<int>(rng() % 4); ++k) {
        at += 1 + static_cast<FrameIndex>(rng() % 20);
        param.keyframes.push_back({at, static_cast<double>(rng() % 100) / 100.0, static_cast<Easing>(rng() % 35)});
    }
    return param;
}

Effect randomEffect(std::mt19937 &rng)
{
    Effect effect;
    effect.service = rng() % 2 ? "brightness" : "volume";
    effect.owner = "testdropin";
    effect.params = {animated(rng, "level"), animated(rng, "gain")};
    return effect;
}

std::vector<EffectId> allEffects(const Model &model)
{
    std::vector<EffectId> ids;
    const Sequence &seq = model.sequence();
    for (const auto &[id, clip] : seq.clips)
        for (const Effect &e : clip.effects)
            ids.push_back(e.id);
    for (const Track &t : seq.tracks)
        for (const Effect &e : t.effects)
            ids.push_back(e.id);
    for (const Effect &e : seq.effects)
        ids.push_back(e.id);
    for (const AdjustmentBlock &b : seq.adjustmentBlocks)
        for (const Effect &e : b.effects)
            ids.push_back(e.id);
    std::sort(ids.begin(), ids.end(), [](EffectId a, EffectId b) { return a.value < b.value; });
    return ids;
}

// Equal but for nextId, which never rolls back (tests/core/test_commands.cpp).
bool sameProject(Project a, Project b)
{
    a.nextId = b.nextId = 0;
    return a == b;
}

} // namespace

TEST_CASE("IP1: random effect and clip commands, undo all and redo all, equal")
{
    for (uint32_t seed : {1u, 2u, 3u}) {
        Model model = Model::createEmpty();
        TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
        Asset asset;
        asset.path = "color:red";
        asset.info.hasVideo = true;
        asset.info.lengthInSequenceFrames = 100'000;
        AssetId assetId = model.addAsset(asset);
        UndoStack undo(model);
        undo.limit = 100'000; // keep every step, so undo all reaches the start
        const Project start = model.project();
        std::mt19937 rng(seed);
        FrameIndex nextPosition = 0;
        int executed = 0;

        for (int i = 0; i < 1500; ++i) {
            std::vector<ClipId> clips(model.track(track).clips.begin(), model.track(track).clips.end());
            std::vector<EffectId> effects = allEffects(model);
            auto pickClip = [&] { return clips[rng() % clips.size()]; };
            auto pickEffect = [&] { return effects[rng() % effects.size()]; };
            std::unique_ptr<Command> command;
            switch (rng() % 14) {
            case 0: {
                FrameIndex length = 20 + static_cast<FrameIndex>(rng() % 80);
                command = std::make_unique<InsertClip>(track, assetId, nextPosition, 0, length - 1);
                nextPosition += length;
                break;
            }
            case 1:
                if (!clips.empty()) {
                    const Clip &clip = model.clip(pickClip());
                    if (clip.length() < 2)
                        break;
                    command = std::make_unique<SplitClip>(
                        clip.id, clip.position + 1 + static_cast<FrameIndex>(rng() % (clip.length() - 1)));
                }
                break;
            case 2:
                if (!clips.empty())
                    command = std::make_unique<RemoveClip>(pickClip());
                break;
            case 3:
            case 4: {
                Target target = Target::sequence();
                if (!clips.empty() && rng() % 3)
                    target = Target::clip(pickClip());
                else if (rng() % 2)
                    target = Target::track(track);
                else if (!model.sequence().adjustmentBlocks.empty())
                    target = Target::adjustmentBlock(model.sequence().adjustmentBlocks.front().id);
                command = std::make_unique<AddEffect>(target, randomEffect(rng), rng() % 3);
                break;
            }
            case 5:
                if (!effects.empty())
                    command = std::make_unique<RemoveEffect>(pickEffect());
                break;
            case 6:
                if (!effects.empty())
                    command = std::make_unique<MoveEffect>(pickEffect(), rng() % 3);
                break;
            case 7:
                if (!effects.empty())
                    command = std::make_unique<SetEffectEnabled>(pickEffect(), rng() % 2);
                break;
            case 8:
                if (!effects.empty())
                    command = std::make_unique<SetParam>(pickEffect(), animated(rng, rng() % 2 ? "level" : "gain"));
                break;
            case 9:
                if (!effects.empty()) {
                    KeyframedValue mix{static_cast<double>(rng() % 101) / 100.0, {}};
                    if (rng() % 2)
                        mix.keyframes = {{0, 0.0, Easing::CubicIn}, {10, 1.0, Easing::Linear}};
                    command = std::make_unique<SetMix>(pickEffect(), mix);
                }
                break;
            case 10:
                if (!effects.empty()) {
                    std::optional<EffectMask> mask;
                    if (rng() % 2)
                        mask = EffectMask{"ellipse", {animated(rng, "rect")}, {0.1, {}}, static_cast<bool>(rng() % 2)};
                    command = std::make_unique<SetMask>(pickEffect(), mask);
                }
                break;
            case 11: {
                AdjustmentBlock block;
                block.lane = static_cast<int>(rng() % 2);
                block.start = static_cast<FrameIndex>(rng() % 500);
                block.length = 1 + static_cast<FrameIndex>(rng() % 100);
                block.effects = {randomEffect(rng)};
                command = std::make_unique<AddAdjustmentBlock>(block);
                break;
            }
            case 12:
                if (!model.sequence().adjustmentBlocks.empty()) {
                    const auto &blocks = model.sequence().adjustmentBlocks;
                    const AdjustmentBlock &block = blocks[rng() % blocks.size()];
                    if (rng() % 3 == 0)
                        command = std::make_unique<RemoveAdjustmentBlock>(block.id);
                    else
                        command = std::make_unique<MoveAdjustmentBlock>(block.id, static_cast<int>(rng() % 2),
                                                                        static_cast<FrameIndex>(rng() % 500),
                                                                        1 + static_cast<FrameIndex>(rng() % 100));
                }
                break;
            case 13:
                if (rng() % 2 || clips.empty())
                    command = std::make_unique<AddLook>(Look{{}, "look", {randomEffect(rng)}});
                else
                    command = std::make_unique<SetClipSource>(pickClip(), std::vector<Param>{animated(rng, "text")});
                break;
            }
            if (!command || !undo.execute(std::move(command)))
                continue;
            ++executed;
            const std::vector<std::string> problems = model.check();
            INFO("seed " << seed << ", step " << i << ": " << (problems.empty() ? "" : problems.front()));
            REQUIRE(problems.empty());
            REQUIRE(*model.snapshot() == model.project());
        }
        const Project end = model.project();
        CHECK(executed > 500);
        while (undo.canUndo())
            undo.undo();
        CHECK(sameProject(model.project(), start));
        while (undo.canRedo())
            undo.redo();
        CHECK(sameProject(model.project(), end));
    }
}

TEST_CASE("IP1: a split gives the right half its own effects, animated from the same time")
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset asset;
    asset.path = "color:red";
    asset.info.hasVideo = true;
    asset.info.lengthInSequenceFrames = 1000;
    ClipId clip = model.insertClip(track, model.addAsset(asset), 0, 0, 99);
    Effect fade;
    fade.service = "brightness";
    Param level;
    level.name = "level";
    level.keyframes = {{0, 0.0, Easing::Linear}, {60, 1.0, Easing::Linear}};
    fade.params = {level};
    UndoStack undo(model);
    REQUIRE(undo.execute(std::make_unique<AddEffect>(Target::clip(clip), fade, 0)));
    auto split = std::make_unique<SplitClip>(clip, 40);
    SplitClip *raw = split.get();
    REQUIRE(undo.execute(std::move(split)));
    const Clip &right = model.clip(raw->rightId());
    REQUIRE(right.effects.size() == 1);
    CHECK(right.effects[0].id != model.clip(clip).effects[0].id);
    CHECK(right.effects[0].params[0].keyframes[0].at == -40);
    CHECK(right.effects[0].params[0].keyframes[1].at == 20);
    CHECK(model.check().empty());
    const EffectId rightEffect = right.effects[0].id;
    undo.undo();
    undo.redo();
    CHECK(model.clip(raw->rightId()).effects[0].id == rightEffect); // the same on redo
}
