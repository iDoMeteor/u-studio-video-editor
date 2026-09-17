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

} // namespace ustudio::core
