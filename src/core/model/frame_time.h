#pragma once

#include <cstdint>

namespace ustudio::core {

// Position/length in SEQUENCE frames (profile fps). Never a double in the
// model -- timecode formatting for display is a view concern (doc 03).
using FrameIndex = int64_t;

struct Rational
{
    int32_t num = 1;
    int32_t den = 1;

    friend bool operator==(const Rational &a, const Rational &b)
    {
        return a.num == b.num && a.den == b.den;
    }
};

struct FrameRange
{
    FrameIndex start = 0;
    FrameIndex length = 0;

    FrameIndex end() const
    {
        return start + length;
    }
};

} // namespace ustudio::core
