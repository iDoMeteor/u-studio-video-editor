#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace ustudio::app::timeline {

// The timeline's vertical geometry: one row per track, top to bottom in
// model order, with a slim name strip at the top of each row and the clips
// below it. A row may carry an extra lane under its clips (doc 15 IP5: a
// drop-in's curve lane or FX lane, TimelineOverlayProvider::laneHeight());
// with no lanes every row is `rowHeight` and the arithmetic is the uniform
// one it always was. Doc 06's per-track heights replace the constants here
// without touching callers.
struct RowLayout
{
    double rowHeight = 60.0;
    double labelHeight = 14.0;
    // Extra height under row i's clips; missing entries are 0.
    std::vector<double> lanes;

    double laneHeight(int row) const
    {
        return row >= 0 && static_cast<size_t>(row) < lanes.size() ? lanes[static_cast<size_t>(row)] : 0.0;
    }
    // Row i's whole height: its clips and its lane.
    double spanOf(int row) const
    {
        return rowHeight + laneHeight(row);
    }
    // May be out of range (negative, or >= the track count); callers clamp.
    int rowAt(double y) const
    {
        if (lanes.empty() || y < 0.0)
            return static_cast<int>(y / rowHeight);
        double top = 0.0;
        for (size_t row = 0; row < lanes.size(); ++row) {
            top += rowHeight + lanes[row];
            if (y < top)
                return static_cast<int>(row);
        }
        return static_cast<int>(lanes.size()) + static_cast<int>((y - top) / rowHeight);
    }
    int clampedRowAt(double y, int trackCount) const
    {
        return std::clamp(rowAt(y), 0, std::max(trackCount - 1, 0));
    }
    double rowTop(int row) const
    {
        double top = row * rowHeight;
        for (int i = 0; i < row && static_cast<size_t>(i) < lanes.size(); ++i)
            top += lanes[static_cast<size_t>(i)];
        return top;
    }
    // The height of `trackCount` rows.
    double contentHeight(int trackCount) const
    {
        return rowTop(trackCount);
    }
    bool inNameStrip(double y) const
    {
        return y - rowTop(rowAt(y)) <= labelHeight;
    }
    // Inside row's lane (below its clips), if it has one.
    bool inLane(double y) const
    {
        const int row = rowAt(y);
        return laneHeight(row) > 0.0 && y >= laneTop(row);
    }
    double clipTop(int row) const
    {
        return rowTop(row) + labelHeight + 2.0;
    }
    double clipHeight() const
    {
        return rowHeight - labelHeight - 6.0;
    }
    double laneTop(int row) const
    {
        return rowTop(row) + rowHeight;
    }
};

} // namespace ustudio::app::timeline
