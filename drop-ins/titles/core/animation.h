#pragma once

// Titles' animation (doc 16, "Animation model"; T3): behaviours expanded
// into offsets, animators and loops; text animators evaluated per unit;
// the typewriter's cursor and the scramble. Pure functions of the layer,
// the timing and the time, so every renderer (the designer's canvas, the
// editor's preview, export) moves identically, and all of it is tested
// without Pango.
//
// Keyframes are core::Keyframe evaluated by core::easedValue, the model
// effects share (docs/developer/notes/animation.md).

#include "title_document.h"

#include <optional>
#include <string>
#include <vector>

namespace ustudio::titles {

// --- The behaviour catalogue ---------------------------------------------------

struct BehaviorInfo
{
    const char *id;
    const char *label;
    bool in, out, loop;        // the slots it fits
    core::FrameIndex duration; // a sensible default (a loop's period)
    core::Easing easing;
    bool textOnly; // needs units of text
};

// Every shipped behaviour, in the drawer's order.
const std::vector<BehaviorInfo> &behaviorCatalogue();
const BehaviorInfo *behaviorInfo(const std::string &id);

// --- Expansion -------------------------------------------------------------------

// A periodic change through the hold (a Loop behaviour).
struct Loop
{
    enum class Shape
    {
        Sine,     // smooth back and forth
        Sawtooth, // -1 to 1, then again (a sweep)
        Noise,    // smooth, seeded wandering
    };
    Property property = Property::Y;
    Shape shape = Shape::Sine;
    double amplitude = 0.0;
    double centre = 0.0; // added to the wave (multiplicative properties: around 1)
    double period = 60.0;
    uint32_t seed = 1;

    bool operator==(const Loop &) const = default;
};

// The scramble: characters show random glyphs until they settle, in order.
struct Scramble
{
    double start = 0.0;  // title frame the first character starts settling
    double spread = 0.0; // frames between the first character settling and the last
    uint32_t seed = 1;
    bool out = false; // reversed: characters dissolve into noise

    bool operator==(const Scramble &) const = default;
};

// The typewriter's cursor: a bar after the last typed character, blinking
// once typing stops, gone at `hide`.
struct Cursor
{
    double start = 0.0, spread = 0.0, hide = 0.0;
    bool out = false; // erasing: the cursor follows the text backwards

    bool operator==(const Cursor &) const = default;
};

// A loop's wave at `sinceHoldStart` frames into the hold (-1..1, before
// its amplitude and centre), and how much loops count at `titleFrame`: 1
// through the hold, ramping from 0 over its first and last 8 frames, 0
// outside it.
double loopWave(const Loop &loop, double sinceHoldStart);
double loopWeight(const Timing &timing, double titleFrame);

// What a layer's behaviours add to it at draw time.
struct Expansion
{
    // Offsets, not values: added to the layer's own (X, Y, Rotation, Blur,
    // Tracking, Shift) or multiplying it (Opacity, Scale, Reveal,
    // ShadowOpacity), so a behaviour moves a layer from wherever it is.
    std::vector<PropertyTrack> offsets;
    std::vector<Animator> animators;
    std::vector<Loop> loops;
    std::optional<Shadow> glow; // glow-breathe on a layer with no shadow of its own
    std::optional<Scramble> scramble;
    std::optional<Cursor> cursor;

    bool operator==(const Expansion &) const = default;
};

Expansion expandBehaviors(const Layer &layer, const Timing &timing);

// Whether `property` combines by multiplying (else by adding).
bool multiplicative(Property property);

// --- Text units --------------------------------------------------------------

// One unit's offsets from its animators, identity when none apply.
struct UnitState
{
    double dx = 0.0, dy = 0.0, scale = 1.0, rotation = 0.0, opacity = 1.0, blur = 0.0;

    bool operator==(const UnitState &) const = default;
};

// Each unit's place in an animator's order: rank[i] is when unit i starts,
// in staggers (centre-out gives the middle 0 and pairs the same rank).
std::vector<double> unitRanks(AnimatorOrder order, size_t count, uint32_t seed);

// The offsets of `count` units of kind `unit` at `titleFrame`, from the
// layer's own animators and its behaviours' (expanded), combined.
std::vector<UnitState> evaluateUnits(const Layer &layer, const Expansion &expansion, const Timing &timing,
                                     double titleFrame, AnimatorUnit unit, size_t count);

// The unit kinds a layer animates (so the renderer splits only as needed).
std::vector<AnimatorUnit> animatedUnits(const Layer &layer, const Expansion &expansion);

// The text as drawn: the scramble's unsettled characters swapped for
// random ones (UTF-8 aware; spaces stay). Unchanged without a scramble.
std::string scrambledText(const std::string &text, const Expansion &expansion, double titleFrame);

// How many of `count` characters the typewriter shows at `titleFrame`, and
// whether its cursor is drawn: nullopt without a typewriter.
struct CursorState
{
    size_t shown = 0;
    bool visible = false;
};
std::optional<CursorState> cursorState(const Expansion &expansion, double titleFrame, size_t count);

// "Detach to keyframes": the layer's in and out behaviours become its own
// keyframes and animators. Loops, the scramble and the typewriter's cursor
// have no keyframe form and stay behaviours.
void detachBehaviors(Layer &layer, const Timing &timing);

// A small, fast PRNG with a fixed algorithm (splitmix64): seeded orders and
// noise must be the same on every platform, which std::shuffle's aren't.
uint64_t mixBits(uint64_t value);

} // namespace ustudio::titles
