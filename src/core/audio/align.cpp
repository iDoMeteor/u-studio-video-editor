#include "align.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ustudio::core::audio {

namespace {

// Pearson correlation of a[i] with b[i - lag] (lag in blocks) where both
// exist. Returns nullopt below minOverlap blocks.
std::optional<double> correlateAt(const std::vector<float> &a, const std::vector<float> &b, int64_t lag,
                                  int64_t minOverlap)
{
    const int64_t na = static_cast<int64_t>(a.size());
    const int64_t nb = static_cast<int64_t>(b.size());
    const int64_t begin = std::max<int64_t>(0, lag);
    const int64_t end = std::min<int64_t>(na, nb + lag);
    const int64_t n = end - begin;
    if (n < minOverlap || n <= 1)
        return std::nullopt;
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    for (int64_t i = begin; i < end; ++i) {
        const double x = a[static_cast<size_t>(i)];
        const double y = b[static_cast<size_t>(i - lag)];
        sa += x;
        sb += y;
        saa += x * x;
        sbb += y * y;
        sab += x * y;
    }
    const double dn = static_cast<double>(n);
    const double cov = sab - sa * sb / dn;
    const double va = saa - sa * sa / dn;
    const double vb = sbb - sb * sb / dn;
    if (va <= 1e-12 || vb <= 1e-12)
        return std::nullopt; // silence (or a constant) can't be lined up
    return cov / std::sqrt(va * vb);
}

std::vector<float> downsample(const std::vector<float> &v, int factor)
{
    std::vector<float> out((v.size() + static_cast<size_t>(factor) - 1) / static_cast<size_t>(factor), 0.0f);
    for (size_t i = 0; i < v.size(); ++i)
        out[i / static_cast<size_t>(factor)] += v[i] / static_cast<float>(factor);
    return out;
}

} // namespace

std::optional<Alignment> align(const Envelope &a, const Envelope &b, double maxShiftMs, double minOverlapMs)
{
    // b's block k sits at b.startMs + k ms now; after a shift s it sits at
    // b.startMs + s + k. Lined up with a's block i when a.startMs + i ==
    // b.startMs + s + k, i.e. i - k == (b.startMs - a.startMs) + s: that
    // difference is the lag correlateAt() takes.
    const double base = b.startMs - a.startMs;

    constexpr int kCoarse = 10;
    const std::vector<float> ca = downsample(a.values, kCoarse);
    const std::vector<float> cb = downsample(b.values, kCoarse);
    const int64_t coarseMax = static_cast<int64_t>(std::ceil(maxShiftMs / kCoarse));
    const int64_t coarseMinOverlap = static_cast<int64_t>(minOverlapMs / kCoarse);

    struct Score
    {
        double shiftMs;
        double correlation;
    };
    std::vector<Score> coarse;
    for (int64_t s = -coarseMax; s <= coarseMax; ++s) {
        const double shiftMs = static_cast<double>(s * kCoarse);
        const int64_t lag = std::llround((base + shiftMs) / kCoarse);
        if (auto c = correlateAt(ca, cb, lag, coarseMinOverlap))
            coarse.push_back({shiftMs, *c});
    }
    if (coarse.empty())
        return std::nullopt;
    const Score best =
        *std::max_element(coarse.begin(), coarse.end(), [](const Score &x, const Score &y) {
            return x.correlation < y.correlation;
        });

    Alignment result;
    result.runnerUp = -1.0;
    for (const Score &s : coarse)
        if (std::abs(s.shiftMs - best.shiftMs) > kSeparationMs)
            result.runnerUp = std::max(result.runnerUp, s.correlation);

    // Confidence is judged at the coarse scale, where best and runner-up
    // are comparable; the refinement only places the shift. Its 1 ms
    // correlations run lower than the smoothed 10 ms ones, so they are
    // compared only with each other.
    result.correlation = best.correlation;
    result.shiftMs = best.shiftMs;
    double bestFine = -2.0;
    const int64_t minOverlap = static_cast<int64_t>(minOverlapMs);
    for (int64_t d = -kCoarse; d <= kCoarse; ++d) {
        const double shiftMs = best.shiftMs + static_cast<double>(d);
        if (std::abs(shiftMs) > maxShiftMs)
            continue;
        const int64_t lag = std::llround(base + shiftMs);
        if (auto c = correlateAt(a.values, b.values, lag, minOverlap); c && *c > bestFine) {
            bestFine = *c;
            result.shiftMs = shiftMs;
        }
    }
    result.confident = result.correlation >= kMinCorrelation && result.correlation - result.runnerUp >= kMinMargin;
    return result;
}

} // namespace ustudio::core::audio
