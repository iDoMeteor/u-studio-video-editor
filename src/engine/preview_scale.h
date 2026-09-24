#pragma once

namespace ustudio::engine {

// How large the *playback* tractor renders, relative to the sequence
// profile (doc 05, "Preview scale"). Export always renders at full size.
enum class PreviewScale
{
    Auto, // Half for sequences taller than 1080 lines, Full otherwise
    Full,
    Half,
    Quarter,
};

// The factor applied to the sequence profile's width and height.
inline double previewScaleFactor(PreviewScale scale, int sequenceHeight)
{
    switch (scale) {
    case PreviewScale::Full:
        return 1.0;
    case PreviewScale::Half:
        return 0.5;
    case PreviewScale::Quarter:
        return 0.25;
    case PreviewScale::Auto:
        break;
    }
    return sequenceHeight > 1080 ? 0.5 : 1.0;
}

} // namespace ustudio::engine
