#pragma once

#include <cmath>

namespace ustudio::core {

// Track::volume (linear gain, 1.0 = unity) as the dB value MLT's "volume"
// filter takes for its "level" property ("adjustment in dB" per the
// filter's YAML; "gain" is the deprecated linear form). Standard 20*log10
// amplitude ratio, confirmed with a standalone repro: a 440 Hz tone through
// a playlist with the filter at -20 dB measured 0.1x peak amplitude.
// Anything at or below a small linear threshold is a fixed silence floor
// rather than log10(0) = -inf. Shared by EngineSync and core/xml's writer
// so a saved project's track levels match playback exactly.
inline double linearToDecibels(double linear)
{
    constexpr double kSilenceFloorDb = -60.0;
    constexpr double kMinLinear = 0.0001;
    if (linear <= kMinLinear)
        return kSilenceFloorDb;
    return 20.0 * std::log10(linear);
}

} // namespace ustudio::core
