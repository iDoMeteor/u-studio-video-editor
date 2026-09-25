#pragma once

#include "core/model/types.h"

#include <string>
#include <utility>
#include <vector>

namespace ustudio::core {

// Doc 13 R7: a new, empty project takes its size and rate from the first
// video imported into it. "Empty": the sequence has no clips and nothing in
// the bin has a picture with a rate (so a still image or an audio file first
// doesn't count, and a second video doesn't re-decide it).
bool sequenceTakesProfileFromMedia(const Project &project);

// `current` with the video's size and rate: square pixels, its own display
// aspect, no stock profile name.
Profile profileForMedia(const Profile &current, int width, int height, Rational fps);

// For an import whose rate differs from the project's: "clip.mp4 is 24 fps;
// the project is 30, so frames will repeat" (or "be skipped"). "" when they
// match or the clip has no rate (a still image).
std::string frameRateNote(const std::string &fileName, Rational clipFps, Rational projectFps);

// The same for a whole import, grouped by rate so it stays one short line:
// "8 are 25 fps; the project is 30, so frames will repeat", or with both
// directions "3 are 25 fps (frames will repeat), 1 is 60 fps (frames will
// be skipped); the project is 30". One differing file reads as
// frameRateNote(). "" when every clip matches (or has no rate).
std::string frameRateSummary(const std::vector<std::pair<std::string, Rational>> &clips, Rational projectFps);

// 24, 23.976, 29.97, 59.94: up to three decimals, no trailing zeros.
std::string formatFps(Rational fps);

} // namespace ustudio::core
