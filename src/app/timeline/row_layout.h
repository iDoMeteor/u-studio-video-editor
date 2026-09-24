#pragma once

#include <algorithm>

namespace ustudio::app::timeline {

// The timeline's vertical geometry: one row per track, top to bottom in
// model order, with a slim name strip at the top of each row and the clips
// below it. Uniform heights for now; doc 06's per-track heights replace
// the constants here without touching callers.
struct RowLayout
{
    double rowHeight = 60.0;
    double labelHeight = 14.0;

    // May be out of range (negative, or >= the track count); callers clamp.
    int rowAt(double y) const
    {
        return static_cast<int>(y / rowHeight);
    }
    int clampedRowAt(double y, int trackCount) const
    {
        return std::clamp(rowAt(y), 0, std::max(trackCount - 1, 0));
    }
    double rowTop(int row) const
    {
        return row * rowHeight;
    }
    bool inNameStrip(double y) const
    {
        return y - rowTop(rowAt(y)) <= labelHeight;
    }
    double clipTop(int row) const
    {
        return rowTop(row) + labelHeight + 2.0;
    }
    double clipHeight() const
    {
        return rowHeight - labelHeight - 6.0;
    }
};

} // namespace ustudio::app::timeline
