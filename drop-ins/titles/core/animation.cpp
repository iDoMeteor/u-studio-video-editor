#include "animation.h"

#include "evaluate.h"

#include "core/model/animation.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ustudio::titles {

namespace {

using core::Easing;
using core::FrameIndex;

// Characters the scramble shows before a character settles.
constexpr std::string_view kScrambleGlyphs = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789#%&*+=?";

TitleKey key(Zone zone, FrameIndex at, double value, Easing easing = Easing::Linear)
{
    return {zone, {at, value, easing}};
}

// An offset going from `from` to `to` over [a0, a1] of `zone`.
void ramp(Expansion &out, Property property, Zone zone, FrameIndex a0, FrameIndex a1, double from, double to,
          Easing easing)
{
    out.offsets.push_back({property, {key(zone, a0, from, easing), key(zone, std::max(a1, a0 + 1), to)}});
}

double identity(Property property)
{
    return multiplicative(property) ? 1.0 : 0.0;
}

// An animator whose units go from `off` to identity (in) or back (out).
Animator unitAnimator(AnimatorUnit unit, AnimatorOrder order, Zone zone, FrameIndex at, double spread,
                      FrameIndex length, const AnimatorKey &off, Easing easing, bool out)
{
    Animator animator;
    animator.unit = unit;
    animator.order = order;
    animator.zone = zone;
    animator.at = at;
    animator.spread = spread;
    AnimatorKey start = out ? AnimatorKey{} : off;
    AnimatorKey end = out ? off : AnimatorKey{};
    start.at = 0;
    start.easing = easing;
    end.at = std::max<FrameIndex>(1, length);
    animator.keys = {start, end};
    return animator;
}

double smoothRamp(double x)
{
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

// Smooth seeded noise in [-1, 1] at `x` (value noise, cosine-interpolated).
double noise(uint32_t seed, double x)
{
    const auto lattice = [seed](int64_t k) {
        const uint64_t bits = mixBits((static_cast<uint64_t>(seed) << 32) ^ static_cast<uint64_t>(k));
        return static_cast<double>(bits >> 11) / static_cast<double>(1ull << 53) * 2.0 - 1.0;
    };
    const double floor = std::floor(x);
    const double t = (1.0 - std::cos((x - floor) * std::numbers::pi)) / 2.0;
    const auto k = static_cast<int64_t>(floor);
    return lattice(k) * (1.0 - t) + lattice(k + 1) * t;
}

// How many UTF-8 code points `text` has that aren't spaces.
size_t visibleCodePoints(const std::string &text)
{
    size_t count = 0;
    for (unsigned char c : text)
        if ((c & 0xC0) != 0x80 && c != ' ' && c != '\n' && c != '\t')
            ++count;
    return count;
}

} // namespace

double loopWave(const Loop &loop, double sinceHoldStart)
{
    const double phase = loop.period > 0.0 ? sinceHoldStart / loop.period : 0.0;
    switch (loop.shape) {
    case Loop::Shape::Sine:
        return std::sin(2.0 * std::numbers::pi * phase);
    case Loop::Shape::Sawtooth:
        return 2.0 * (phase - std::floor(phase)) - 1.0;
    case Loop::Shape::Noise:
        return noise(loop.seed, phase);
    }
    return 0.0;
}

double loopWeight(const Timing &timing, double titleFrame)
{
    constexpr double kRamp = 8.0;
    const auto holdStart = static_cast<double>(timing.intro);
    const auto holdEnd = static_cast<double>(timing.intro + timing.hold);
    if (titleFrame <= holdStart || titleFrame >= holdEnd)
        return 0.0;
    return smoothRamp(std::min(titleFrame - holdStart, holdEnd - titleFrame) / kRamp);
}

uint64_t mixBits(uint64_t value)
{
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

bool multiplicative(Property property)
{
    switch (property) {
    case Property::Opacity:
    case Property::Scale:
    case Property::Reveal:
    case Property::ShadowOpacity:
        return true;
    default:
        return false;
    }
}

const std::vector<BehaviorInfo> &behaviorCatalogue()
{
    using E = Easing;
    static const std::vector<BehaviorInfo> catalogue = {
        // id, label, in, out, loop, duration, easing, text only
        {"fade", "Fade", true, true, false, 12, E::SinusoidalOut, false},
        {"rise", "Rise", true, true, false, 15, E::CubicOut, false},
        {"drop", "Drop", true, true, false, 15, E::CubicOut, false},
        {"pop", "Pop", true, true, false, 12, E::BackOut, false},
        {"typewriter", "Typewriter", true, true, false, 30, E::Linear, true},
        {"word-by-word", "Word by word", true, true, false, 24, E::CubicOut, true},
        {"blur", "Blur", true, true, false, 15, E::CubicOut, false},
        {"wipe", "Wipe", true, true, false, 15, E::CubicInOut, false},
        {"scramble", "Scramble", true, true, false, 30, E::Linear, true},
        {"split-lines", "Split lines", true, true, false, 20, E::CubicOut, true},
        // Not back_out: MLT's overshoots by ~37% of the change, which from
        // 1.8x dips lines to 0.7x mid-way.
        {"kinetic-stack", "Kinetic stack", true, true, false, 24, E::CubicOut, true},
        {"collapse", "Collapse", false, true, false, 12, E::CubicIn, false}, // back_in swells ~37% first
        {"float", "Float", false, false, true, 90, E::Linear, false},
        {"pulse", "Pulse", false, false, true, 45, E::Linear, false},
        {"shimmer", "Shimmer", false, false, true, 60, E::Linear, false},
        {"wiggle", "Wiggle", false, false, true, 10, E::Linear, false},
        {"glow-breathe", "Glow breathe", false, false, true, 60, E::Linear, false},
    };
    return catalogue;
}

const BehaviorInfo *behaviorInfo(const std::string &id)
{
    for (const BehaviorInfo &info : behaviorCatalogue())
        if (id == info.id)
            return &info;
    return nullptr;
}

Expansion expandBehaviors(const Layer &layer, const Timing &timing)
{
    Expansion out;
    for (const Behavior &b : layer.behaviors) {
        const double a = b.amount;
        const FrameIndex d = std::max<FrameIndex>(1, b.duration);
        if (b.slot == BehaviorSlot::Loop) {
            const auto period = static_cast<double>(d);
            if (b.id == "float") {
                out.loops.push_back({Property::Y, Loop::Shape::Sine, 10.0 * a, 0.0, period, b.seed});
            } else if (b.id == "pulse") {
                out.loops.push_back({Property::Scale, Loop::Shape::Sine, 0.04 * a, 1.0, period, b.seed});
            } else if (b.id == "shimmer") {
                out.loops.push_back({Property::Shift, Loop::Shape::Sawtooth, 1.0, 0.0, period, b.seed});
            } else if (b.id == "wiggle") {
                out.loops.push_back({Property::Rotation, Loop::Shape::Noise, 2.5 * a, 0.0, period, b.seed});
                out.loops.push_back({Property::X, Loop::Shape::Noise, 3.0 * a, 0.0, period, b.seed + 1});
                out.loops.push_back({Property::Y, Loop::Shape::Noise, 3.0 * a, 0.0, period, b.seed + 2});
            } else if (b.id == "glow-breathe") {
                if (!layer.shadow.enabled) {
                    Shadow glow;
                    glow.enabled = true;
                    glow.dx = glow.dy = 0.0;
                    glow.blur = 18.0 * a;
                    glow.color = layer.fill.kind == FillKind::Solid ? layer.fill.color : layer.fill.from;
                    glow.opacity = 1.0;
                    out.glow = glow;
                }
                out.loops.push_back({Property::ShadowOpacity, Loop::Shape::Sine, 0.35, 0.65, period, b.seed});
            }
            continue;
        }

        const bool isOut = b.slot == BehaviorSlot::Out;
        // In: the intro's first d frames; out: the outro's last d.
        const Zone zone = isOut ? Zone::Outro : Zone::Intro;
        const FrameIndex a0 = isOut ? std::max<FrameIndex>(0, timing.outro - d) : 0;
        const FrameIndex a1 = a0 + d;
        const auto offset = [&](Property p, double off) {
            ramp(out, p, zone, a0, a1, isOut ? identity(p) : off, isOut ? off : identity(p), b.easing);
        };
        const double start = keyPosition(timing, {zone, {a0, 0.0, Easing::Linear}});

        if (b.id == "fade") {
            offset(Property::Opacity, 0.0);
        } else if (b.id == "rise") {
            offset(Property::Y, isOut ? -40.0 * a : 40.0 * a);
            offset(Property::Opacity, 0.0);
        } else if (b.id == "drop") {
            offset(Property::Y, isOut ? 60.0 * a : -60.0 * a);
            offset(Property::Opacity, 0.0);
        } else if (b.id == "pop") {
            offset(Property::Scale, 0.5);
            offset(Property::Opacity, 0.0);
        } else if (b.id == "blur") {
            offset(Property::Blur, 24.0 * a);
            offset(Property::Opacity, 0.0);
        } else if (b.id == "wipe") {
            offset(Property::Reveal, 0.0);
        } else if (b.id == "collapse") {
            offset(Property::Scale, 0.0);
            offset(Property::Opacity, 0.0);
        } else if (b.id == "typewriter") {
            AnimatorKey off;
            off.opacity = 0.0;
            // A discrete step: a character is there or not.
            Animator typed =
                unitAnimator(AnimatorUnit::Character, isOut ? AnimatorOrder::Reverse : AnimatorOrder::Forward, zone, a0,
                             static_cast<double>(d), 1, off, Easing::Discrete, isOut);
            out.animators.push_back(typed);
            const double hide =
                isOut ? start + static_cast<double>(d) + 1.0 : static_cast<double>(timing.intro + timing.hold);
            out.cursor = Cursor{start, static_cast<double>(d), hide, isOut};
        } else if (b.id == "word-by-word") {
            AnimatorKey off;
            off.opacity = 0.0;
            off.dy = isOut ? -24.0 * a : 24.0 * a;
            const auto length = static_cast<FrameIndex>(std::lround(static_cast<double>(d) * 0.5));
            out.animators.push_back(unitAnimator(AnimatorUnit::Word, AnimatorOrder::Forward, zone, a0,
                                                 static_cast<double>(d - length), length, off, b.easing, isOut));
        } else if (b.id == "split-lines") {
            AnimatorKey off;
            off.opacity = 0.0;
            off.dx = isOut ? 240.0 * a : -240.0 * a;
            const auto length = static_cast<FrameIndex>(std::lround(static_cast<double>(d) * 0.7));
            Animator split = unitAnimator(AnimatorUnit::Line, AnimatorOrder::Forward, zone, a0,
                                          static_cast<double>(d - length), length, off, b.easing, isOut);
            split.alternate = true;
            out.animators.push_back(split);
        } else if (b.id == "kinetic-stack") {
            AnimatorKey off;
            off.opacity = 0.0;
            off.scale = isOut ? 0.4 : 1.8;
            off.dy = isOut ? 40.0 * a : -40.0 * a;
            const auto length = static_cast<FrameIndex>(std::lround(static_cast<double>(d) * 0.6));
            out.animators.push_back(unitAnimator(AnimatorUnit::Line, AnimatorOrder::Forward, zone, a0,
                                                 static_cast<double>(d - length), length, off, b.easing, isOut));
        } else if (b.id == "scramble") {
            out.scramble = Scramble{start, static_cast<double>(d) * 0.8, b.seed, isOut};
            // A quick fade so the noise doesn't pop in (or out).
            const FrameIndex fade = std::min<FrameIndex>(d, 6);
            if (isOut)
                ramp(out, Property::Opacity, zone, a1 - fade, a1, 1.0, 0.0, Easing::Linear);
            else
                ramp(out, Property::Opacity, zone, a0, a0 + fade, 0.0, 1.0, Easing::Linear);
        }
    }
    return out;
}

std::vector<double> unitRanks(AnimatorOrder order, size_t count, uint32_t seed)
{
    std::vector<double> ranks(count, 0.0);
    switch (order) {
    case AnimatorOrder::Forward:
        for (size_t i = 0; i < count; ++i)
            ranks[i] = static_cast<double>(i);
        break;
    case AnimatorOrder::Reverse:
        for (size_t i = 0; i < count; ++i)
            ranks[i] = static_cast<double>(count - 1 - i);
        break;
    case AnimatorOrder::CentreOut: {
        const double centre = (static_cast<double>(count) - 1.0) / 2.0;
        for (size_t i = 0; i < count; ++i)
            ranks[i] = std::floor(std::abs(static_cast<double>(i) - centre));
        break;
    }
    case AnimatorOrder::Random: {
        // Fisher-Yates with mixBits: the same shuffle on every platform.
        std::vector<size_t> shuffled(count);
        for (size_t i = 0; i < count; ++i)
            shuffled[i] = i;
        uint64_t state = seed;
        for (size_t i = count; i > 1; --i) {
            state = mixBits(state);
            std::swap(shuffled[i - 1], shuffled[state % i]);
        }
        for (size_t position = 0; position < count; ++position)
            ranks[shuffled[position]] = static_cast<double>(position);
        break;
    }
    }
    return ranks;
}

std::vector<UnitState> evaluateUnits(const Layer &layer, const Expansion &expansion, const Timing &timing,
                                     double titleFrame, AnimatorUnit unit, size_t count)
{
    std::vector<UnitState> units(count);
    const auto apply = [&](const Animator &animator) {
        if (animator.unit != unit || animator.keys.empty() || count == 0)
            return;
        const std::vector<double> ranks = unitRanks(animator.order, count, animator.seed);
        const double maxRank = *std::max_element(ranks.begin(), ranks.end());
        const double stagger = animator.spread > 0.0 ? animator.spread / std::max(1.0, maxRank) : animator.stagger;
        const double start = keyPosition(timing, {animator.zone, {animator.at, 0.0, Easing::Linear}});
        // One curve per channel, on core's keyframe model.
        std::array<std::vector<core::Keyframe>, 6> channels;
        for (const AnimatorKey &k : animator.keys) {
            const double values[6] = {k.dx, k.dy, k.scale, k.rotation, k.opacity, k.blur};
            for (size_t c = 0; c < 6; ++c)
                channels[c].push_back({k.at, values[c], k.easing});
        }
        for (size_t i = 0; i < count; ++i) {
            const double t = titleFrame - (start + ranks[i] * stagger);
            UnitState &u = units[i];
            const double dx = core::easedValue(channels[0], t);
            u.dx += animator.alternate && (i % 2 == 1) ? -dx : dx;
            u.dy += core::easedValue(channels[1], t);
            u.scale *= core::easedValue(channels[2], t);
            u.rotation += core::easedValue(channels[3], t);
            u.opacity *= std::clamp(core::easedValue(channels[4], t), 0.0, 1.0);
            u.blur += core::easedValue(channels[5], t);
        }
    };
    for (const Animator &animator : layer.animators)
        apply(animator);
    for (const Animator &animator : expansion.animators)
        apply(animator);
    return units;
}

std::vector<AnimatorUnit> animatedUnits(const Layer &layer, const Expansion &expansion)
{
    std::vector<AnimatorUnit> kinds;
    const auto note = [&](const Animator &a) {
        if (std::find(kinds.begin(), kinds.end(), a.unit) == kinds.end())
            kinds.push_back(a.unit);
    };
    for (const Animator &a : layer.animators)
        note(a);
    for (const Animator &a : expansion.animators)
        note(a);
    return kinds;
}

std::string scrambledText(const std::string &text, const Expansion &expansion, double titleFrame)
{
    if (!expansion.scramble)
        return text;
    const Scramble &s = *expansion.scramble;
    const size_t count = visibleCodePoints(text);
    const auto tick = static_cast<uint64_t>(std::max(0.0, std::floor(titleFrame / 2.0)));
    std::string out;
    size_t index = 0;
    for (size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        const size_t length = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : 4;
        const std::string_view glyph(text.data() + i, std::min(length, text.size() - i));
        i += glyph.size();
        if (glyph == " " || glyph == "\n" || glyph == "\t") {
            out += glyph;
            continue;
        }
        // Character `index` settles at start + its share of the spread.
        const double share = count > 1 ? static_cast<double>(index) / static_cast<double>(count - 1) : 0.0;
        const double moment = s.start + share * s.spread;
        const bool noisy = s.out ? titleFrame >= moment : titleFrame < moment;
        if (noisy) {
            const uint64_t bits = mixBits((static_cast<uint64_t>(s.seed) << 40) ^ (tick << 20) ^ index);
            out += kScrambleGlyphs[bits % kScrambleGlyphs.size()];
        } else {
            out += glyph;
        }
        ++index;
    }
    return out;
}

std::optional<CursorState> cursorState(const Expansion &expansion, double titleFrame, size_t count)
{
    if (!expansion.cursor)
        return std::nullopt;
    const Cursor &c = *expansion.cursor;
    // As the typewriter's animator: character i shows from start + i*stagger + 1
    // (erasing: goes at start + (count-1-i)*stagger + 1).
    const double stagger = c.spread / std::max(1.0, static_cast<double>(count) - 1.0);
    CursorState state;
    for (size_t i = 0; i < count; ++i) {
        const double rank = c.out ? static_cast<double>(count - 1 - i) : static_cast<double>(i);
        const bool typed = titleFrame >= c.start + rank * stagger + 1.0;
        if (c.out ? !typed : typed)
            ++state.shown;
    }
    const double done = c.start + c.spread + 1.0;
    if (titleFrame >= c.start && titleFrame < c.hide) {
        // Solid while typing; blinking (8 frames on, 8 off) once it stops.
        state.visible = titleFrame < done || static_cast<int64_t>(std::floor((titleFrame - done) / 8.0)) % 2 == 0;
    }
    return state;
}

void detachBehaviors(Layer &layer, const Timing &timing)
{
    // Only in and out behaviours with a keyframe form.
    Layer detached = layer;
    detached.behaviors.clear();
    std::vector<Behavior> kept;
    for (const Behavior &b : layer.behaviors) {
        if (b.slot == BehaviorSlot::Loop || b.id == "typewriter" || b.id == "scramble")
            kept.push_back(b);
        else
            detached.behaviors.push_back(b);
    }
    const Expansion expansion = expandBehaviors(detached, timing);
    detached.behaviors.clear();

    for (const PropertyTrack &offset : expansion.offsets) {
        auto own = std::find_if(detached.animation.begin(), detached.animation.end(),
                                [&](const PropertyTrack &t) { return t.property == offset.property; });
        // The offset's keys become absolute values: on the layer's own value
        // there (its keyframes, or its base value), combined.
        PropertyTrack merged{offset.property, {}};
        std::vector<TitleKey> positions = offset.keys;
        if (own != detached.animation.end())
            positions.insert(positions.end(), own->keys.begin(), own->keys.end());
        std::stable_sort(positions.begin(), positions.end(), [&](const TitleKey &x, const TitleKey &y) {
            return keyPosition(timing, x) < keyPosition(timing, y);
        });
        Layer withoutOffset = detached; // its own value, before this offset
        for (const TitleKey &position : positions) {
            const double at = keyPosition(timing, position);
            if (!merged.keys.empty() && keyPosition(timing, merged.keys.back()) == at)
                continue;
            const LayerState base = evaluateLayer(withoutOffset, timing, at);
            std::vector<core::Keyframe> offsetKeys;
            for (const TitleKey &k : offset.keys)
                offsetKeys.push_back({static_cast<FrameIndex>(keyPosition(timing, k)), k.key.value, k.key.easing});
            const double off = core::easedValue(offsetKeys, at);
            const double value = stateValue(base, offset.property);
            TitleKey k = position;
            k.key.value = multiplicative(offset.property) ? value * off : value + off;
            merged.keys.push_back(k);
        }
        if (own != detached.animation.end())
            *own = merged;
        else
            detached.animation.push_back(merged);
    }
    detached.animators.insert(detached.animators.end(), expansion.animators.begin(), expansion.animators.end());
    detached.behaviors = kept;
    layer = std::move(detached);
}

} // namespace ustudio::titles
