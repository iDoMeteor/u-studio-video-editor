#pragma once

#include "core/model/types.h"

namespace ustudio::core {

// The project with its active sequence moved to `fps`: every frame index
// (clip positions and source ranges, dissolves, fades, keyframes, markers)
// scaled by fps / old fps, so everything stays where it was in *time*.
// Absolute positions are rounded, never lengths: a cut lands within half a
// frame of its true time however many clips precede it, and butted clips
// stay butted (rounding lengths instead drifts 0.6 s over 10 minutes at
// 24 -> 23.976; doc 12, "Frame rate"). Lengths follow from the rounded
// ends. The caller's project is untouched; the result passes Model::check()
// whenever the input does, except that a dissolve rounding to nothing is
// dropped (a cut in its place).
//
// Used by render (a profile's frame rate, on a snapshot) and by the
// sequence frame-rate change.
Project retime(const Project &project, Rational fps);

// round(frame * to / from), exactly, for non-negative frames.
FrameIndex retimeFrame(FrameIndex frame, Rational from, Rational to);

} // namespace ustudio::core
