// core::easedValue() is the one keyframe evaluator for code that animates
// in-process (the titles drop-in's renderer, cut-edge values in
// core::keyframesForCut()). It must give what MLT gives for the same
// animation string, so a title keyframe and an effect keyframe with the same
// easing move the same way: pinned here against Mlt::Properties::
// anim_get_double() for every Easing.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/model/animation.h"
#include "engine/factory_policy.h"

#include <mlt++/Mlt.h>

#include <cmath>
#include <string>
#include <vector>

using namespace ustudio;
using namespace ustudio::core;

namespace {

// MLT's value for `keyframes` at `frame`, through its animation string.
double mltValue(const std::vector<Keyframe> &keyframes, int frame, int length)
{
    Mlt::Properties properties;
    properties.set("v", animationString(keyframes).c_str());
    return properties.anim_get_double("v", frame, length);
}

const engine::FactoryPolicy &factory()
{
    static engine::FactoryPolicy policy;
    return policy;
}

} // namespace

TEST_CASE("easedValue matches MLT for every easing, every frame")
{
    factory();
    for (int e = static_cast<int>(Easing::Discrete); e <= static_cast<int>(Easing::BounceInOut); ++e) {
        const auto easing = static_cast<Easing>(e);
        // Four keyframes, so the smooth kinds see both a duplicated end and
        // real neighbours, a peak (100 then 20) and a non-zero start.
        const std::vector<Keyframe> keyframes = {
            {5, 10.0, easing}, {25, 100.0, easing}, {40, 20.0, easing}, {70, 60.0, easing}};
        const int length = 80;
        for (int frame = 0; frame < length; ++frame) {
            CAPTURE(e);
            CAPTURE(frame);
            CHECK(easedValue(keyframes, frame) == doctest::Approx(mltValue(keyframes, frame, length)).epsilon(1e-9));
        }
    }
}

TEST_CASE("easedValue: edges, a single keyframe, none, and fractional frames")
{
    const std::vector<Keyframe> keyframes = {{10, 1.0, Easing::Linear}, {20, 3.0, Easing::Linear}};
    CHECK(easedValue(keyframes, 0) == 1.0);
    CHECK(easedValue(keyframes, 10) == 1.0);
    CHECK(easedValue(keyframes, 15.5) == doctest::Approx(2.1));
    CHECK(easedValue(keyframes, 20) == 3.0);
    CHECK(easedValue(keyframes, 99) == 3.0);
    CHECK(easedValue({{4, 7.0, Easing::CubicIn}}, 0) == 7.0);
    CHECK(easedValue({{4, 7.0, Easing::CubicIn}}, 9) == 7.0);
    CHECK(easedValue({}, 3) == 0.0);
    // Discrete holds until the next keyframe, then jumps.
    const std::vector<Keyframe> discrete = {{0, 1.0, Easing::Discrete}, {10, 5.0, Easing::Linear}};
    CHECK(easedValue(discrete, 9.9) == 1.0);
    CHECK(easedValue(discrete, 10) == 5.0);
}

TEST_CASE("keyframesForCut's edge keyframes carry MLT's eased value there")
{
    factory();
    for (Easing easing : {Easing::Linear, Easing::CubicIn, Easing::BackOut, Easing::SmoothNatural, Easing::BounceInOut,
                          Easing::Discrete}) {
        CAPTURE(static_cast<int>(easing));
        const std::vector<Keyframe> keyframes = {{0, 0.0, easing}, {30, 90.0, easing}, {60, 30.0, easing}};
        // A cut from frame 12 to 44 of the owner: both edges fall inside
        // segments, so both are synthesised.
        const FrameIndex offset = 12, length = 33;
        const std::vector<Keyframe> cut = keyframesForCut(keyframes, offset, length);
        REQUIRE(cut.size() == 3);
        CHECK(cut.front().at == 0);
        CHECK(cut.front().value == doctest::Approx(mltValue(keyframes, 12, 61)).epsilon(1e-9));
        CHECK(cut[1].at == 18);
        CHECK(cut[1].value == 90.0);
        CHECK(cut.back().at == 32);
        CHECK(cut.back().value == doctest::Approx(mltValue(keyframes, 44, 61)).epsilon(1e-9));
    }
}

TEST_CASE("easing names round-trip, and unknown names are refused")
{
    for (int e = static_cast<int>(Easing::Discrete); e <= static_cast<int>(Easing::BounceInOut); ++e) {
        const auto easing = static_cast<Easing>(e);
        CAPTURE(e);
        CHECK(easingFromName(easingName(easing)) == easing);
        CHECK(easingFromOperator(easingOperator(easing)[0]) == easing);
    }
    CHECK(easingName(Easing::CubicOut) == std::string("cubic_out"));
    CHECK_FALSE(easingFromName("cubic").has_value());
}
