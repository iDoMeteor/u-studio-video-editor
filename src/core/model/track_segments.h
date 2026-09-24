#pragma once

#include "model.h"

#include <vector>

namespace ustudio::core {

// One entry of a track's MLT playlist, in order. A dissolve between clips
// `a` and `b` replaces what would otherwise be two whole-clip Clip segments
// with up to three: `a`'s own exclusive (pre-overlap) span, the Transition
// sub-tractor, and `b`'s exclusive (post-overlap) span. Either exclusive
// span is omitted entirely if a clip's whole length is consumed by the
// transition(s) touching it.
//
// Shared by EngineSync (builds the live Mlt::Playlist and checks it in
// verify()) and core/xml's writer (serialises the same structure into the
// project file's render playlists, so `melt` or u-studio-render plays a
// saved project exactly as the editor does -- doc 12, M1). One planner, so
// the two can't drift apart on what a track's playlist looks like.
struct TrackSegment
{
    enum class Kind
    {
        Clip,
        Transition,
    } kind;

    FrameIndex start;  // position on the track
    FrameIndex length; // frame count, always > 0

    // Kind::Clip: `in`/`out` are this segment's own source range --
    // narrower than the model clip's full in/out when a transition has
    // trimmed the head and/or tail off it.
    ClipId clip;
    FrameIndex in = 0, out = 0;

    // Kind::Transition
    TransitionId transition;
    ClipId a, b;
};

std::vector<TrackSegment> planTrackSegments(const Model &model, const Track &track);

} // namespace ustudio::core
