#include "core/model/animation.h"

#include <array>
#include <charconv>
#include <cmath>
#include <numbers>

namespace ustudio::core {

namespace {
// Indexed by Easing's value, which is mlt_keyframe_type's.
constexpr std::array<const char *, 35> kOperators = {
    "|", "",  "~", "$", "-", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
    "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "A", "B", "C", "D",
};
static_assert(static_cast<size_t>(Easing::BounceInOut) + 1 == kOperators.size());
constexpr std::array<const char *, 35> kNames = {
    "discrete",
    "linear",
    "smooth",
    "smooth_natural",
    "smooth_tight",
    "sinusoidal_in",
    "sinusoidal_out",
    "sinusoidal_in_out",
    "quadratic_in",
    "quadratic_out",
    "quadratic_in_out",
    "cubic_in",
    "cubic_out",
    "cubic_in_out",
    "quartic_in",
    "quartic_out",
    "quartic_in_out",
    "quintic_in",
    "quintic_out",
    "quintic_in_out",
    "exponential_in",
    "exponential_out",
    "exponential_in_out",
    "circular_in",
    "circular_out",
    "circular_in_out",
    "back_in",
    "back_out",
    "back_in_out",
    "elastic_in",
    "elastic_out",
    "elastic_in_out",
    "bounce_in",
    "bounce_out",
    "bounce_in_out",
};

// mlt_animation.c's easing formulas (MLT 7.40, after Robert Penner's), as
// the fraction of the way from one value to the next at progress t.
enum class Ease
{
    In,
    Out,
    InOut
};

double sinusoidal(double t, Ease ease)
{
    if (ease == Ease::In)
        return std::sin((t - 1) * std::numbers::pi / 2) + 1;
    if (ease == Ease::Out)
        return std::sin(t * std::numbers::pi / 2);
    return 0.5 * (1 - std::cos(t * std::numbers::pi));
}

double power(double t, double order, Ease ease)
{
    if (ease == Ease::In)
        return std::pow(t, order);
    if (ease == Ease::Out)
        return 1 - std::pow(1 - t, order);
    if (t < 0.5)
        return std::pow(2, order) * std::pow(t, order) / 2;
    return 1.0 - std::pow(-2 * t + 2, order) / 2;
}

double exponential(double t, Ease ease)
{
    if (t == 0.0)
        return 0.0;
    if (t == 1.0)
        return 1.0;
    if (ease == Ease::In)
        return std::pow(2.0, 10 * t - 10);
    if (ease == Ease::Out)
        return 1.0 - std::pow(2.0, -10 * t);
    if (t < 0.5)
        return std::pow(2, 20 * t - 10) / 2;
    return (2 - std::pow(2, -20 * t + 10)) / 2;
}

double circular(double t, Ease ease)
{
    if (ease == Ease::In)
        return 1.0 - std::sqrt(1.0 - t * t);
    if (ease == Ease::Out)
        return std::sqrt(1.0 - (t - 1.0) * (t - 1.0));
    if (t < 0.5)
        return 0.5 * (1 - std::sqrt(1 - 4 * (t * t)));
    return 0.5 * (std::sqrt(-((2 * t) - 3) * ((2 * t) - 1)) + 1);
}

double back(double t, Ease ease)
{
    const auto curve = [](double f) { return f * f * f - f * std::sin(f * std::numbers::pi); };
    if (ease == Ease::In)
        return curve(t);
    if (ease == Ease::Out)
        return 1 - curve(1 - t);
    if (t < 0.5)
        return 0.5 * curve(2 * t);
    return 0.5 * (1 - curve(1 - (2 * t - 1))) + 0.5;
}

double elastic(double t, Ease ease)
{
    const double k = 13 * std::numbers::pi / 2;
    if (ease == Ease::In)
        return std::sin(k * t) * std::pow(2, 10 * (t - 1));
    if (ease == Ease::Out)
        return std::sin(-k * (t + 1)) * std::pow(2, -10 * t) + 1;
    if (t < 0.5)
        return 0.5 * std::sin(k * (2 * t)) * std::pow(2, 10 * ((2 * t) - 1));
    return 0.5 * (std::sin(-k * ((2 * t - 1) + 1)) * std::pow(2, -10 * (2 * t - 1)) + 2);
}

double bounce(double t, Ease ease)
{
    if (ease == Ease::In)
        return 1.0 - bounce(1.0 - t, Ease::Out);
    if (ease == Ease::Out) {
        if (t < 4 / 11.0)
            return (121 * t * t) / 16.0;
        if (t < 8 / 11.0)
            return (363 / 40.0 * t * t) - (99 / 10.0 * t) + 17 / 5.0;
        if (t < 9 / 10.0)
            return (4356 / 361.0 * t * t) - (35442 / 1805.0 * t) + 16061 / 1805.0;
        return (54 / 5.0 * t * t) - (513 / 25.0 * t) + 268 / 25.0;
    }
    if (t < 0.5)
        return 0.5 * bounce(t * 2, Ease::In);
    return 0.5 * bounce(2.0 * t - 1.0, Ease::Out) + 0.5;
}

// mlt_animation.c's catmull_rom_interpolate(): the segment (x1,y1)-(x2,y2)
// with (x0,y0) and (x3,y3) as control points; a duplicated end point is
// moved 10000 frames away, making that end flat.
double catmullRom(double x0, double y0, double x1, double y1, double x2, double y2, double x3, double y3, double t,
                  double alpha, double tension)
{
    if (x0 == x1)
        x0 -= 10000;
    if (x3 == x2)
        x3 += 10000;
    const auto distance = [](double ax, double ay, double bx, double by) { return std::hypot(bx - ax, by - ay); };
    double m1 = 0;
    double m2 = 0;
    const double t12 = std::pow(distance(x1, y1, x2, y2), alpha);
    if (tension > 0.0 || (y1 < y0 && y1 > y2) || (y1 > y0 && y1 < y2)) {
        const double t01 = std::pow(distance(x0, y0, x1, y1), alpha);
        m1 = std::fabs(tension) * (y2 - y1 + t12 * ((y1 - y0) / t01 - (y2 - y0) / (t01 + t12)));
    }
    if (tension > 0.0 || (y2 < y1 && y2 > y3) || (y2 > y1 && y2 < y3)) {
        const double t23 = std::pow(distance(x2, y2, x3, y3), alpha);
        m2 = std::fabs(tension) * (y2 - y1 + t12 * ((y3 - y2) / t23 - (y3 - y1) / (t12 + t23)));
    }
    const double a = 2.0 * (y1 - y2) + m1 + m2;
    const double b = -3.0 * (y1 - y2) - m1 - m1 - m2;
    return a * t * t * t + b * t * t + m1 * t + y1;
}

// The fraction of the way from one value to the next, for the non-smooth
// easings (the smooth ones need the neighbours: catmullRom()).
double easeFraction(Easing easing, double t)
{
    switch (easing) {
    case Easing::Discrete:
        return 0.0;
    case Easing::Linear:
    case Easing::SmoothLoose:
    case Easing::SmoothNatural:
    case Easing::SmoothTight:
        return t;
    case Easing::SinusoidalIn:
        return sinusoidal(t, Ease::In);
    case Easing::SinusoidalOut:
        return sinusoidal(t, Ease::Out);
    case Easing::SinusoidalInOut:
        return sinusoidal(t, Ease::InOut);
    case Easing::QuadraticIn:
        return power(t, 2, Ease::In);
    case Easing::QuadraticOut:
        return power(t, 2, Ease::Out);
    case Easing::QuadraticInOut:
        return power(t, 2, Ease::InOut);
    case Easing::CubicIn:
        return power(t, 3, Ease::In);
    case Easing::CubicOut:
        return power(t, 3, Ease::Out);
    case Easing::CubicInOut:
        return power(t, 3, Ease::InOut);
    case Easing::QuarticIn:
        return power(t, 4, Ease::In);
    case Easing::QuarticOut:
        return power(t, 4, Ease::Out);
    case Easing::QuarticInOut:
        return power(t, 4, Ease::InOut);
    case Easing::QuinticIn:
        return power(t, 5, Ease::In);
    case Easing::QuinticOut:
        return power(t, 5, Ease::Out);
    case Easing::QuinticInOut:
        return power(t, 5, Ease::InOut);
    case Easing::ExponentialIn:
        return exponential(t, Ease::In);
    case Easing::ExponentialOut:
        return exponential(t, Ease::Out);
    case Easing::ExponentialInOut:
        return exponential(t, Ease::InOut);
    case Easing::CircularIn:
        return circular(t, Ease::In);
    case Easing::CircularOut:
        return circular(t, Ease::Out);
    case Easing::CircularInOut:
        return circular(t, Ease::InOut);
    case Easing::BackIn:
        return back(t, Ease::In);
    case Easing::BackOut:
        return back(t, Ease::Out);
    case Easing::BackInOut:
        return back(t, Ease::InOut);
    case Easing::ElasticIn:
        return elastic(t, Ease::In);
    case Easing::ElasticOut:
        return elastic(t, Ease::Out);
    case Easing::ElasticInOut:
        return elastic(t, Ease::InOut);
    case Easing::BounceIn:
        return bounce(t, Ease::In);
    case Easing::BounceOut:
        return bounce(t, Ease::Out);
    case Easing::BounceInOut:
        return bounce(t, Ease::InOut);
    }
    return t;
}
} // namespace

const char *easingOperator(Easing easing)
{
    const size_t index = static_cast<size_t>(easing);
    return index < kOperators.size() ? kOperators[index] : "";
}

Easing easingFromOperator(char op)
{
    for (size_t i = 0; i < kOperators.size(); ++i)
        if (kOperators[i][0] != '\0' && kOperators[i][0] == op)
            return static_cast<Easing>(i);
    if (op == '!')
        return Easing::Discrete; // MLT's other spelling of discrete
    return Easing::Linear;
}

double easedValue(const std::vector<Keyframe> &keyframes, double frame)
{
    if (keyframes.empty())
        return 0.0;
    // The last keyframe at or before `frame` (MLT walks the same way).
    size_t i = 0;
    while (i + 1 < keyframes.size() && frame >= static_cast<double>(keyframes[i + 1].at))
        ++i;
    const Keyframe &k1 = keyframes[i];
    if (frame <= static_cast<double>(k1.at) || i + 1 == keyframes.size())
        return k1.value;
    const Keyframe &k0 = i > 0 ? keyframes[i - 1] : k1;
    const Keyframe &k2 = keyframes[i + 1];
    const Keyframe &k3 = i + 2 < keyframes.size() ? keyframes[i + 2] : k2;
    const double t = (frame - static_cast<double>(k1.at)) / static_cast<double>(k2.at - k1.at);
    const auto x = [](const Keyframe &k) { return static_cast<double>(k.at); };
    switch (k1.easing) {
    case Easing::SmoothLoose:
        return catmullRom(x(k0), k0.value, x(k1), k1.value, x(k2), k2.value, x(k3), k3.value, t, 0.0, 1.0);
    case Easing::SmoothNatural:
        return catmullRom(x(k0), k0.value, x(k1), k1.value, x(k2), k2.value, x(k3), k3.value, t, 0.5, -1.0);
    case Easing::SmoothTight:
        return catmullRom(x(k0), k0.value, x(k1), k1.value, x(k2), k2.value, x(k3), k3.value, t, 0.5, 0.0);
    default:
        return k1.value + (k2.value - k1.value) * easeFraction(k1.easing, t);
    }
}

const char *easingName(Easing easing)
{
    const size_t index = static_cast<size_t>(easing);
    return index < kNames.size() ? kNames[index] : "linear";
}

std::optional<Easing> easingFromName(std::string_view name)
{
    for (size_t i = 0; i < kNames.size(); ++i)
        if (name == kNames[i])
            return static_cast<Easing>(i);
    return std::nullopt;
}

std::vector<Keyframe> keyframesForCut(const std::vector<Keyframe> &keyframes, FrameIndex offset, FrameIndex length)
{
    std::vector<Keyframe> cut;
    if (keyframes.empty() || length <= 0)
        return cut;
    const FrameIndex last = length - 1;
    for (size_t i = 0; i < keyframes.size(); ++i) {
        Keyframe k = keyframes[i];
        k.at -= offset;
        if (k.at < 0) {
            // The last keyframe before the cut sets the value at its start.
            const bool nextInside = i + 1 < keyframes.size() && keyframes[i + 1].at - offset > 0;
            if (i + 1 == keyframes.size() || nextInside) {
                Keyframe edge = k;
                edge.at = 0;
                edge.value = easedValue(keyframes, static_cast<double>(offset));
                cut.push_back(edge);
            }
            continue;
        }
        if (k.at > last) {
            // The first keyframe past the cut sets the value at its end.
            if (!cut.empty() && cut.back().at < last) {
                Keyframe edge = cut.back();
                edge.value = easedValue(keyframes, static_cast<double>(offset + last));
                edge.at = last;
                cut.push_back(edge);
            } else if (cut.empty()) {
                Keyframe edge = k;
                edge.at = 0;
                cut.push_back(edge);
            }
            break;
        }
        cut.push_back(k);
    }
    return cut;
}

std::string formatDouble(double value)
{
    std::array<char, 64> buf{};
    auto result = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    return std::string(buf.data(), result.ptr);
}

std::string animationString(const std::vector<Keyframe> &keyframes)
{
    std::string out;
    for (const Keyframe &k : keyframes) {
        if (!out.empty())
            out += ';';
        out += std::to_string(k.at) + easingOperator(k.easing) + "=" + formatDouble(k.value);
    }
    return out;
}

} // namespace ustudio::core
