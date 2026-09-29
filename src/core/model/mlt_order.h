#pragma once

#include "types.h"

#include <vector>

namespace ustudio::core {

// The order tracks appear in the MLT tractor (doc 03): audio tracks first,
// in model order, then video tracks bottom-to-top. The model stores
// tracks in visual top-to-bottom order (index 0 = top), so video tracks
// are reversed here to get ascending MLT indices. Does not include the
// black backing track (MLT index 0, not a model track) -- callers add
// that themselves.
//
// Shared by EngineSync (projects the model into a live Mlt::Tractor) and
// core/xml's writer (serialises the same ordering into the project file's
// MLT-facing <tractor>, so `melt`/`u-studio-render` sees an identical
// graph with no editor involved).
std::vector<TrackId> mltTrackOrder(const Sequence &sequence);

// FX4: the tractor's layers once adjustment blocks sit below the first row.
// A block on lane k > 0 affects only the video rows at and below k, so
// those rows form a sub-tractor G(k) whose track 0 is the next deeper G (or
// the background), and G(k) is the next layer's track 0. `lanes` are the
// lanes > 0 that have a block and a video row at or below them, ascending;
// `layerTracks[0]` is the main tractor's tracks (audio, and video above the
// first lane) and `layerTracks[i + 1]` G(lanes[i])'s, each in
// mltTrackOrder()'s order. With no such lane it's one layer, mltTrackOrder().
// Shared by EngineSync and the writer, like mltTrackOrder().
struct AdjustmentLayers
{
    std::vector<int> lanes;
    std::vector<std::vector<TrackId>> layerTracks;
};
AdjustmentLayers adjustmentLayers(const Sequence &sequence);

} // namespace ustudio::core
