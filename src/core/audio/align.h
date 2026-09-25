#pragma once

#include <optional>
#include <vector>

namespace ustudio::core::audio {

// A clip's loudness over time: the mean absolute amplitude of its (mono)
// audio in consecutive blocks of kBlockMs, the first block starting at
// `startMs` on the timeline. engine/audio_sync.cpp decodes these.
struct Envelope
{
    static constexpr double kBlockMs = 1.0;
    double startMs = 0.0;
    std::vector<float> values;
};

struct Alignment
{
    double shiftMs = 0.0;      // add to b's timeline position to line it up with a
    double correlation = 0.0;  // at shiftMs, -1..1
    double runnerUp = 0.0;     // the best clearly different shift (> kSeparationMs away)
    bool confident = false;    // correlation and its margin over runnerUp are both good enough
};

// The shift within +/- maxShiftMs that best lines b's envelope up with a's
// (normalised cross-correlation over where they overlap, so a quieter or
// louder recording of the same sound matches as well). Coarse (10 ms) over
// the whole range, then 1 ms around the best. Nullopt if no shift leaves
// them overlapping for at least minOverlapMs. Pure: runs on a pool thread.
std::optional<Alignment> align(const Envelope &a, const Envelope &b, double maxShiftMs, double minOverlapMs);

// Below these a match is reported, not trusted (Alignment::confident).
constexpr double kMinCorrelation = 0.5;
constexpr double kMinMargin = 0.1;
constexpr double kSeparationMs = 100.0;

} // namespace ustudio::core::audio
