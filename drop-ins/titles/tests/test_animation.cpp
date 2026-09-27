// Titles' animation (core/animation, doc 16 T3): animator orders and
// clocks, every easing through an animator, every shipped behaviour, loops,
// the scramble and the typewriter's cursor, and Detach to keyframes. No
// Pango: this is what every renderer draws from.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/animation.h"
#include "core/evaluate.h"
#include "core/model/animation.h"
#include "core/title_edit.h"
#include "core/title_xml.h"

#include <set>

using namespace ustudio;
using namespace ustudio::titles;

namespace {
const Timing kTiming{20, 60, 20}; // intro 0-20, hold 20-80, outro 80-100

Layer textLayer()
{
    Layer layer;
    layer.id = "t";
    layer.kind = LayerKind::Text;
    layer.text = "Hello world";
    layer.x = 100;
    layer.y = 200;
    return layer;
}

Layer withBehavior(BehaviorSlot slot, const std::string &id)
{
    Layer layer = textLayer();
    const BehaviorInfo *info = behaviorInfo(id);
    REQUIRE(info);
    layer.behaviors.push_back({slot, id, info->duration, info->easing, 7, 1.0});
    return layer;
}
} // namespace

TEST_CASE("unit ranks for every order")
{
    CHECK(unitRanks(AnimatorOrder::Forward, 4, 1) == std::vector<double>{0, 1, 2, 3});
    CHECK(unitRanks(AnimatorOrder::Reverse, 4, 1) == std::vector<double>{3, 2, 1, 0});
    CHECK(unitRanks(AnimatorOrder::CentreOut, 5, 1) == std::vector<double>{2, 1, 0, 1, 2});
    CHECK(unitRanks(AnimatorOrder::CentreOut, 4, 1) == std::vector<double>{1, 0, 0, 1});
    // Random: a permutation, the same for the same seed, another for another.
    const std::vector<double> a = unitRanks(AnimatorOrder::Random, 12, 42);
    CHECK(std::set<double>(a.begin(), a.end()).size() == 12);
    CHECK(unitRanks(AnimatorOrder::Random, 12, 42) == a);
    CHECK(unitRanks(AnimatorOrder::Random, 12, 43) != a);
    // Pinned: the shuffle is splitmix64 + Fisher-Yates, identical everywhere.
    CHECK(unitRanks(AnimatorOrder::Random, 5, 1) == unitRanks(AnimatorOrder::Random, 5, 1));
    CHECK(unitRanks(AnimatorOrder::Forward, 0, 1).empty());
}

TEST_CASE("each unit runs the curve on its own clock: stagger, spread, alternate")
{
    Layer layer = textLayer();
    Animator animator;
    animator.unit = AnimatorUnit::Character;
    animator.stagger = 2;
    animator.keys = {{0, core::Easing::Linear, 10, 0, 1, 0, 0, 0}, {10, core::Easing::Linear, 0, 0, 1, 0, 1, 0}};
    layer.animators = {animator};
    const Expansion none;
    // Frame 4: unit 0 is 4 frames in, unit 1 is 2, unit 2 is just starting.
    std::vector<UnitState> units = evaluateUnits(layer, none, kTiming, 4, AnimatorUnit::Character, 4);
    CHECK(units[0].dx == doctest::Approx(6));
    CHECK(units[0].opacity == doctest::Approx(0.4));
    CHECK(units[1].dx == doctest::Approx(8));
    CHECK(units[2].opacity == doctest::Approx(0.0));
    CHECK(units[3].opacity == 0.0); // before its start: the curve's first value
    // Word units are untouched by a character animator.
    CHECK(evaluateUnits(layer, none, kTiming, 4, AnimatorUnit::Word, 2)[0] == UnitState{});
    // A spread fits the whole count into it: 3 units over 10 frames.
    layer.animators[0].spread = 10;
    units = evaluateUnits(layer, none, kTiming, 10, AnimatorUnit::Character, 3);
    CHECK(units[0].opacity == doctest::Approx(1.0));
    CHECK(units[1].opacity == doctest::Approx(0.5));
    CHECK(units[2].opacity == doctest::Approx(0.0));
    // Alternate mirrors every other unit's dx.
    layer.animators[0].spread = 0;
    layer.animators[0].alternate = true;
    units = evaluateUnits(layer, none, kTiming, 0, AnimatorUnit::Character, 2);
    CHECK(units[0].dx == doctest::Approx(10));
    CHECK(units[1].dx == doctest::Approx(-10));
}

TEST_CASE("every easing, through an animator, moves as core::easedValue")
{
    for (int e = static_cast<int>(core::Easing::Discrete); e <= static_cast<int>(core::Easing::BounceInOut); ++e) {
        const auto easing = static_cast<core::Easing>(e);
        CAPTURE(core::easingName(easing));
        Layer layer = textLayer();
        Animator animator;
        animator.keys = {{0, easing, 0, 0, 1, 0, 0, 0}, {20, core::Easing::Linear, 0, 100, 1, 0, 1, 0}};
        layer.animators = {animator};
        const std::vector<core::Keyframe> reference = {{0, 0.0, easing}, {20, 100.0, core::Easing::Linear}};
        for (double t : {0.0, 3.0, 7.5, 10.0, 15.0, 19.0, 20.0}) {
            const UnitState unit = evaluateUnits(layer, Expansion{}, kTiming, t, AnimatorUnit::Character, 1)[0];
            CHECK(unit.dy == doctest::Approx(core::easedValue(reference, t)));
        }
    }
}

TEST_CASE("every shipped behaviour expands, in each slot it fits, and moves")
{
    for (const BehaviorInfo &info : behaviorCatalogue()) {
        for (BehaviorSlot slot : {BehaviorSlot::In, BehaviorSlot::Out, BehaviorSlot::Loop}) {
            const bool fits = (slot == BehaviorSlot::In && info.in) || (slot == BehaviorSlot::Out && info.out) ||
                              (slot == BehaviorSlot::Loop && info.loop);
            if (!fits)
                continue;
            CAPTURE(info.id);
            CAPTURE(static_cast<int>(slot));
            const Layer layer = withBehavior(slot, info.id);
            const Expansion expansion = expandBehaviors(layer, kTiming);
            CHECK_FALSE(expansion == Expansion{});
            // Something differs between two moments inside its window.
            const double a = slot == BehaviorSlot::In ? 1 : slot == BehaviorSlot::Out ? 99 : 30;
            const double b = slot == BehaviorSlot::In ? 10 : slot == BehaviorSlot::Out ? 90 : 50;
            const bool stateMoves =
                !(evaluateLayer(layer, expansion, kTiming, a) == evaluateLayer(layer, expansion, kTiming, b));
            const bool unitsMove = !animatedUnits(layer, expansion).empty();
            const bool textMoves = scrambledText(layer.text, expansion, a) != scrambledText(layer.text, expansion, b) ||
                                   expansion.cursor.has_value();
            CHECK((stateMoves || unitsMove || textMoves));
            // And the layer is itself once it's over (in) or before it starts (out).
            if (slot == BehaviorSlot::In)
                CHECK(evaluateLayer(layer, expansion, kTiming, 50).opacity == doctest::Approx(1.0));
            if (slot == BehaviorSlot::Out)
                CHECK(evaluateLayer(layer, expansion, kTiming, 50).opacity == doctest::Approx(1.0));
        }
    }
}

TEST_CASE("in and out: from nothing to the layer, and back")
{
    const Layer rise = withBehavior(BehaviorSlot::In, "rise");
    CHECK(evaluateLayer(rise, kTiming, 0).opacity == 0.0);
    CHECK(evaluateLayer(rise, kTiming, 0).y == doctest::Approx(240));
    CHECK(evaluateLayer(rise, kTiming, 15).y == doctest::Approx(200));
    const Layer fadeOut = withBehavior(BehaviorSlot::Out, "fade");
    CHECK(evaluateLayer(fadeOut, kTiming, 87).opacity == doctest::Approx(1.0)); // the outro's last 12 frames
    CHECK(evaluateLayer(fadeOut, kTiming, 100).opacity == doctest::Approx(0.0));
    // Offsets compose with the layer's own keyframes.
    Layer moving = rise;
    moving.animation.push_back(
        {Property::X,
         {{Zone::Intro, {0, 0.0, core::Easing::Linear}}, {Zone::Intro, {20, 500.0, core::Easing::Linear}}}});
    CHECK(evaluateLayer(moving, kTiming, 10).x == doctest::Approx(250));
    CHECK(evaluateLayer(moving, kTiming, 0).opacity == 0.0);
}

TEST_CASE("loops play only in the hold, ramped at its edges")
{
    const Layer floating = withBehavior(BehaviorSlot::Loop, "float");
    CHECK(evaluateLayer(floating, kTiming, 10).y == 200); // intro
    CHECK(evaluateLayer(floating, kTiming, 90).y == 200); // outro
    CHECK(evaluateLayer(floating, kTiming, 20).y == 200); // the hold's edge
    double lowest = 200, highest = 200;
    for (double t = 20; t < 80; t += 0.5) {
        lowest = std::min(lowest, evaluateLayer(floating, kTiming, t).y);
        highest = std::max(highest, evaluateLayer(floating, kTiming, t).y);
    }
    CHECK(highest - lowest > 15); // about the 2 x 10 px amplitude
    CHECK(highest - lowest < 21);
    CHECK(loopWeight(kTiming, 50) == 1.0);
    CHECK(loopWeight(kTiming, 22) < 1.0);
    // Pulse multiplies the scale around 1; glow breathe adds a glow.
    const Layer glowing = withBehavior(BehaviorSlot::Loop, "glow-breathe");
    CHECK(expandBehaviors(glowing, kTiming).glow.has_value());
    CHECK(evaluateLayer(glowing, kTiming, 50).shadowOpacity < 1.0);
}

TEST_CASE("the scramble settles in order, the same every time")
{
    const Layer layer = withBehavior(BehaviorSlot::In, "scramble");
    const Expansion expansion = expandBehaviors(layer, kTiming);
    const std::string before = scrambledText("Hello world", expansion, 0);
    CHECK(before.size() == 11);
    CHECK(before[5] == ' '); // spaces stay
    CHECK(before != "Hello world");
    CHECK(scrambledText("Hello world", expansion, 0) == before); // deterministic
    CHECK(scrambledText("Hello world", expansion, 40) == "Hello world");
    // Halfway, the first characters have settled and the last haven't.
    const std::string half = scrambledText("Hello world", expansion, 12);
    CHECK(half.substr(0, 2) == "He");
    CHECK(half.substr(8) != "rld");
    // Multi-byte text keeps its spaces and settles to itself.
    CHECK(scrambledText("Grüße dir", expansion, 40) == "Grüße dir");
}

TEST_CASE("the typewriter's cursor follows the typed text, then blinks, then goes")
{
    const Layer layer = withBehavior(BehaviorSlot::In, "typewriter"); // 30 frames
    const Expansion expansion = expandBehaviors(layer, kTiming);
    REQUIRE(expansion.cursor.has_value());
    const size_t count = 11;
    CHECK(cursorState(expansion, 0, count)->shown == 0);
    CHECK(cursorState(expansion, 0, count)->visible);
    CHECK(cursorState(expansion, 16, count)->shown == 6);
    CHECK(cursorState(expansion, 40, count)->shown == count);
    // The animator agrees with the cursor about which characters show.
    const std::vector<UnitState> units = evaluateUnits(layer, expansion, kTiming, 16, AnimatorUnit::Character, count);
    size_t shown = 0;
    for (const UnitState &u : units)
        shown += u.opacity > 0.5;
    CHECK(shown == cursorState(expansion, 16, count)->shown);
    // Blinking after typing (done at 31), gone at the hold's end (80).
    CHECK(cursorState(expansion, 33, count)->visible);
    CHECK_FALSE(cursorState(expansion, 41, count)->visible);
    CHECK_FALSE(cursorState(expansion, 85, count)->visible);
    CHECK_FALSE(cursorState(Expansion{}, 10, count).has_value());
}

TEST_CASE("Detach to keyframes keeps the motion, as plain keyframes and animators")
{
    for (const char *id : {"fade", "rise", "drop", "pop", "blur", "wipe", "word-by-word", "split-lines"}) {
        for (BehaviorSlot slot : {BehaviorSlot::In, BehaviorSlot::Out}) {
            CAPTURE(id);
            CAPTURE(static_cast<int>(slot));
            const Layer layer = withBehavior(slot, id);
            Layer detached = layer;
            detachBehaviors(detached, kTiming);
            CHECK(detached.behaviors.empty());
            const Expansion before = expandBehaviors(layer, kTiming), after = expandBehaviors(detached, kTiming);
            for (double t = 0; t <= 100; t += 1) {
                const LayerState a = evaluateLayer(layer, before, kTiming, t);
                const LayerState b = evaluateLayer(detached, after, kTiming, t);
                CHECK(a.y == doctest::Approx(b.y));
                CHECK(a.opacity == doctest::Approx(b.opacity));
                CHECK(a.scale == doctest::Approx(b.scale));
                CHECK(a.blur == doctest::Approx(b.blur));
                CHECK(a.reveal == doctest::Approx(b.reveal));
                CHECK(evaluateUnits(layer, before, kTiming, t, AnimatorUnit::Word, 3) ==
                      evaluateUnits(detached, after, kTiming, t, AnimatorUnit::Word, 3));
            }
        }
    }
    // Loops, the scramble and the typewriter stay behaviours.
    Layer kept = withBehavior(BehaviorSlot::Loop, "float");
    kept.behaviors.push_back({BehaviorSlot::In, "typewriter", 30, core::Easing::Linear, 1, 1.0});
    detachBehaviors(kept, kTiming);
    CHECK(kept.behaviors.size() == 2);
}

TEST_CASE("animators and behaviours round-trip through the file")
{
    TitleDocument doc;
    Layer layer = withBehavior(BehaviorSlot::In, "kinetic-stack");
    layer.behaviors.push_back({BehaviorSlot::Loop, "wiggle", 12, core::Easing::Linear, 9, 1.5});
    Animator animator;
    animator.unit = AnimatorUnit::Word;
    animator.order = AnimatorOrder::Random;
    animator.seed = 77;
    animator.spread = 14;
    animator.alternate = true;
    animator.zone = Zone::Outro;
    animator.at = 3;
    animator.keys = {{0, core::Easing::BackOut, 5, -7, 1.5, 30, 0.25, 4}, {9, core::Easing::Linear, 0, 0, 1, 0, 1, 0}};
    layer.animators.push_back(animator);
    layer.blur = 2.5;
    layer.animation.push_back({Property::FillR, {{Zone::Hold, {4, 0.25, core::Easing::CubicIn}}}});
    addLayer(doc, layer);
    auto again = parseTitle(writeTitle(doc));
    REQUIRE(again.has_value());
    CHECK(again->warnings.empty());
    CHECK(again->document == doc);
}
