#pragma once

// IP6: `u-studio-render --title-export <title.ustitle> <output> <format>
// [--seconds S] [--field name=value]...`. A title on its own, for OBS and
// other tools (doc 16, T2d; owner requirement 2026-09-27):
//
//   png     a folder of PNGs with alpha (<output>/<name>_00001.png ...)
//   prores  ProRes 4444 with alpha (.mov, yuva444p10le)
//   qtrle   QuickTime Animation with alpha (.mov, argb)
//   webm    VP9 with alpha (.webm, yuva420p)
//   h264    flattened onto the title's background, or black (.mp4)
//
// The title's own frames go straight to the encoder: nothing composites
// them, so the alpha is exactly the renderer's. Written atomically (a
// .part file or folder, renamed when done). Prints one JSON line: the
// output and its frame count, or an error.

#include <iosfwd>
#include <string>
#include <vector>

namespace ustudio::titles {

int runTitleExport(const std::vector<std::string> &args, std::ostream &out);

} // namespace ustudio::titles
